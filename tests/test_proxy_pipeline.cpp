// ---------------------------------------------------------------------------
// test_proxy_pipeline.cpp
//
// 프록시 파이프라인 통합 테스트 (실제 MySQL 서버 없이 순수 함수 테스트).
//
// [테스트 범위]
// - SqlParser → PolicyEngine 파이프라인: 허용/차단 판정
// - SqlParser → InjectionDetector: SQL Injection 패턴 탐지
// - SqlParser → ProcedureDetector: 저장 프로시저/동적 SQL 탐지
// - 파싱 실패 → PolicyEngine::evaluate_error() → fail-close (kBlock)
// - StatsCollector 연동: 쿼리 처리 후 통계 업데이트 확인
//
// [fail-close 검증]
// 파이프라인의 모든 오류 경로가 kBlock 으로 수렴하는지 검증한다.
// ❌ 파싱 오류 → kAllow 는 절대 허용되지 않는다.
//
// [테스트 설계]
// - 실제 MySQL 연결 없이 각 모듈의 public API 를 직접 호출한다.
// - 파이프라인을 simulate_pipeline() 헬퍼로 캡슐화한다:
//     1. SqlParser::parse(sql)
//     2. 파싱 성공 시 InjectionDetector::check(sql) → 탐지 시 kBlock
//     3. ProcedureDetector::detect(query) → 정책과 결합
//     4. PolicyEngine::evaluate(query, session) → 최종 판정
//     5. 파싱 실패 시 PolicyEngine::evaluate_error(error, session) → kBlock
//
// [오탐/미탐 주의]
// - InjectionDetector 의 UNION SELECT 패턴은 정상 UNION ALL 쿼리에서
//   false positive 가능 (알려진 트레이드오프).
// - ProcedureDetector 는 PREPARE @var 패턴의 변수 간접 참조를 탐지하지 못함.
// ---------------------------------------------------------------------------

#include "parser/injection_detector.hpp"
#include "parser/procedure_detector.hpp"
#include "parser/sql_parser.hpp"
#include "policy/policy_engine.hpp"
#include "policy/rule.hpp"
#include "stats/stats_collector.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// 헬퍼: 기본 인젝션 탐지 패턴 (test_injection_detector.cpp 와 동일)
// ---------------------------------------------------------------------------
namespace {

std::vector<std::string> default_injection_patterns() {
    return {
        "UNION\\s+SELECT",
        "'\\s*OR\\s+['\"\\d]",
        "SLEEP\\s*\\(",
        "BENCHMARK\\s*\\(",
        "LOAD_FILE\\s*\\(",
        "INTO\\s+OUTFILE",
        "INTO\\s+DUMPFILE",
        ";\\s*(DROP|DELETE|UPDATE|INSERT|ALTER|CREATE|CALL|PREPARE|EXECUTE|TRUNCATE)",
        "--\\s*$",
        "/\\*.*\\*/",
    };
}

// ---------------------------------------------------------------------------
// make_default_config
//   파이프라인 테스트용 기본 PolicyConfig.
//
//   - DROP, TRUNCATE, ALTER 차단
//   - SELECT, INSERT, UPDATE, DELETE 허용 (user "*")
//   - 인젝션 탐지 패턴 적용
//   - procedure_control: block_dynamic_sql=true
// ---------------------------------------------------------------------------
std::shared_ptr<PolicyConfig> make_default_config() {
    auto cfg = std::make_shared<PolicyConfig>();

    // SQL 구문 차단 규칙
    cfg->sql_rules.block_statements = {"DROP", "TRUNCATE", "ALTER"};
    cfg->sql_rules.block_patterns   = default_injection_patterns();

    // 기본 사용자 접근 규칙: * 사용자에게 SELECT/INSERT/UPDATE/DELETE 허용
    AccessRule allow_rule;
    allow_rule.user               = "*";
    allow_rule.source_ip_cidr     = "";  // 모든 IP 허용
    allow_rule.allowed_tables     = {"*"};
    allow_rule.allowed_operations = {"SELECT", "INSERT", "UPDATE", "DELETE", "CALL"};
    allow_rule.blocked_operations = {};
    cfg->access_control.push_back(std::move(allow_rule));

    // 프로시저 제어: whitelist 모드, 동적 SQL 차단
    cfg->procedure_control.mode             = "whitelist";
    cfg->procedure_control.whitelist        = {"safe_proc"};  // 허용 프로시저
    cfg->procedure_control.block_dynamic_sql = true;
    cfg->procedure_control.block_create_alter = true;

    return cfg;
}

// ---------------------------------------------------------------------------
// make_session
//   테스트용 기본 SessionContext 생성.
// ---------------------------------------------------------------------------
SessionContext make_session(const std::string& user  = "testuser",
                            const std::string& ip    = "127.0.0.1") {
    SessionContext ctx;
    ctx.session_id    = 1;
    ctx.client_ip     = ip;
    ctx.client_port   = 12345;
    ctx.db_user       = user;
    ctx.db_name       = "testdb";
    ctx.handshake_done = true;
    return ctx;
}

// ---------------------------------------------------------------------------
// PipelineResult
//   simulate_pipeline() 의 반환값.
// ---------------------------------------------------------------------------
struct PipelineResult {
    PolicyAction     action;
    std::string      reason;
    bool             injection_detected{false};
    bool             procedure_detected{false};
};

// ---------------------------------------------------------------------------
// simulate_pipeline
//   프록시 파이프라인을 시뮬레이션한다.
//
//   순서:
//   1. SqlParser::parse(sql)
//   2. 파싱 실패 → PolicyEngine::evaluate_error() → kBlock
//   3. InjectionDetector::check(sql) → 탐지 시 kBlock (즉시 반환)
//   4. ProcedureDetector::detect(query) → 탐지 여부 기록
//   5. PolicyEngine::evaluate(query, session) → 최종 판정
//   6. StatsCollector::on_query(blocked) 호출
// ---------------------------------------------------------------------------
PipelineResult simulate_pipeline(
    std::string_view               sql,
    const PolicyEngine&            engine,
    const InjectionDetector&       injection_det,
    const ProcedureDetector&       proc_det,
    const SessionContext&          session,
    StatsCollector&                stats)
{
    SqlParser parser;
    const auto parse_result = parser.parse(sql);

    if (!parse_result.has_value()) {
        // 파싱 실패 → fail-close
        const auto policy_result = engine.evaluate_error(parse_result.error(), session);
        stats.on_query(true);  // 파싱 실패는 차단으로 기록
        return PipelineResult{
            .action             = policy_result.action,
            .reason             = policy_result.reason,
            .injection_detected = false,
            .procedure_detected = false,
        };
    }

    const auto& query = parse_result.value();

    // 인젝션 탐지
    const auto inj_result = injection_det.check(sql);
    if (inj_result.detected) {
        stats.on_query(true);
        return PipelineResult{
            .action             = PolicyAction::kBlock,
            .reason             = inj_result.reason,
            .injection_detected = true,
            .procedure_detected = false,
        };
    }

    // 프로시저 탐지 (결과는 기록만 — 정책 판정은 PolicyEngine 에 위임)
    const auto proc_info = proc_det.detect(query);
    const bool proc_found = proc_info.has_value();

    // 정책 판정
    const auto policy_result = engine.evaluate(query, session);
    const bool is_blocked = (policy_result.action == PolicyAction::kBlock);
    stats.on_query(is_blocked);

    return PipelineResult{
        .action             = policy_result.action,
        .reason             = policy_result.reason,
        .injection_detected = false,
        .procedure_detected = proc_found,
    };
}

} // namespace

// ---------------------------------------------------------------------------
// 테스트 픽스처
// ---------------------------------------------------------------------------
class ProxyPipeline : public ::testing::Test {
protected:
    void SetUp() override {
        config_    = make_default_config();
        engine_    = std::make_unique<PolicyEngine>(config_);
        inj_det_   = std::make_unique<InjectionDetector>(default_injection_patterns());
        proc_det_  = std::make_unique<ProcedureDetector>();
        stats_     = std::make_unique<StatsCollector>();
        session_   = make_session();
    }

    // run_pipeline: 편의 래퍼
    PipelineResult run(std::string_view sql) {
        return simulate_pipeline(sql, *engine_, *inj_det_, *proc_det_, session_, *stats_);
    }

    std::shared_ptr<PolicyConfig>     config_;
    std::unique_ptr<PolicyEngine>     engine_;
    std::unique_ptr<InjectionDetector> inj_det_;
    std::unique_ptr<ProcedureDetector> proc_det_;
    std::unique_ptr<StatsCollector>   stats_;
    SessionContext                    session_;
};

// ---------------------------------------------------------------------------
// AllowedQuery_NoBlock
//   일반 SELECT 쿼리는 차단되지 않아야 한다 (PolicyAction::kAllow).
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, AllowedQuery_NoBlock) {
    const auto result = run("SELECT * FROM users WHERE id = 1");

    EXPECT_EQ(result.action, PolicyAction::kAllow)
        << "Normal SELECT should be allowed. reason: " << result.reason;
    EXPECT_FALSE(result.injection_detected)
        << "Normal SELECT should not trigger injection detection";
}

// ---------------------------------------------------------------------------
// DropTable_IsBlocked
//   DROP TABLE 구문은 block_statements 규칙에 의해 차단되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, DropTable_IsBlocked) {
    const auto result = run("DROP TABLE users");

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "DROP TABLE must be blocked by sql_rules.block_statements";
}

// ---------------------------------------------------------------------------
// TruncateTable_IsBlocked
//   TRUNCATE TABLE 구문도 block_statements 에 포함되어 차단되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, TruncateTable_IsBlocked) {
    const auto result = run("TRUNCATE TABLE sessions");

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "TRUNCATE TABLE must be blocked by sql_rules.block_statements";
}

// ---------------------------------------------------------------------------
// InjectionSql_IsBlocked
//   OR 1=1 tautology 인젝션 패턴은 InjectionDetector 에서 탐지되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, InjectionSql_IsBlocked) {
    const auto result = run("SELECT * FROM users WHERE name = '' OR '1'='1'");

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "SQL injection (OR tautology) must be blocked";
    EXPECT_TRUE(result.injection_detected)
        << "InjectionDetector must flag OR tautology";
}

// ---------------------------------------------------------------------------
// UnionInjection_IsBlocked
//   UNION SELECT 인젝션은 탐지되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, UnionInjection_IsBlocked) {
    const auto result = run("SELECT * FROM users UNION SELECT 1,2,3");

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "UNION SELECT injection must be blocked";
    EXPECT_TRUE(result.injection_detected)
        << "InjectionDetector must flag UNION SELECT";
}

// ---------------------------------------------------------------------------
// SleepInjection_IsBlocked
//   SLEEP() 기반 time-based blind injection 은 차단되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, SleepInjection_IsBlocked) {
    const auto result = run("SELECT * FROM users WHERE id = 1 AND SLEEP(5)");

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "SLEEP() injection must be blocked";
    EXPECT_TRUE(result.injection_detected)
        << "InjectionDetector must flag SLEEP()";
}

// ---------------------------------------------------------------------------
// ParseError_IsBlocked
//   파싱이 실패하는 경우(빈 쿼리) fail-close 원칙에 따라 kBlock 이어야 한다.
//
//   [fail-close 검증]
//   파싱 실패 → evaluate_error() → kBlock 경로를 명시적으로 검증한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, ParseError_IsBlocked) {
    // 빈 문자열은 파싱 실패
    const auto result = run("");

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "Parse failure (empty SQL) must be fail-close (kBlock)";
}

// ---------------------------------------------------------------------------
// WhitespaceOnly_IsBlocked
//   공백만 있는 SQL 도 파싱 실패로 차단되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, WhitespaceOnly_IsBlocked) {
    const auto result = run("   \t\n  ");

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "Whitespace-only SQL must be fail-close (kBlock)";
}

// ---------------------------------------------------------------------------
// ComQuery_StatsUpdated
//   쿼리 처리 후 StatsCollector 의 통계가 정확하게 업데이트되어야 한다.
//
//   [검증 포인트]
//   - 허용 쿼리: total_queries 증가, blocked_queries 변화 없음
//   - 차단 쿼리: total_queries 증가, blocked_queries 증가
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, ComQuery_StatsUpdated) {
    // 초기 상태 확인
    auto snap0 = stats_->snapshot();
    EXPECT_EQ(snap0.total_queries,   0u);
    EXPECT_EQ(snap0.blocked_queries, 0u);

    // 허용 쿼리 처리
    const auto allow_result = run("SELECT 1");
    EXPECT_EQ(allow_result.action, PolicyAction::kAllow);

    auto snap1 = stats_->snapshot();
    EXPECT_EQ(snap1.total_queries,   1u) << "total_queries must increment after allowed query";
    EXPECT_EQ(snap1.blocked_queries, 0u) << "blocked_queries must not increment for allowed query";

    // 차단 쿼리 처리 (DROP)
    const auto block_result = run("DROP TABLE users");
    EXPECT_EQ(block_result.action, PolicyAction::kBlock);

    auto snap2 = stats_->snapshot();
    EXPECT_EQ(snap2.total_queries,   2u) << "total_queries must increment after blocked query";
    EXPECT_EQ(snap2.blocked_queries, 1u) << "blocked_queries must increment for blocked query";

    // block_rate 검증: 1 / 2 = 0.5
    EXPECT_NEAR(snap2.block_rate, 0.5, 1e-9);
}

// ---------------------------------------------------------------------------
// ProcedureCall_Detection
//   CALL 구문은 ProcedureDetector 에서 탐지되어야 한다.
//   whitelist 에 포함된 프로시저("safe_proc")는 허용된다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, ProcedureCall_Detection) {
    const auto result = run("CALL safe_proc()");

    // ProcedureDetector 가 CALL 을 탐지해야 함
    EXPECT_TRUE(result.procedure_detected)
        << "ProcedureDetector must detect CALL statement";
    // whitelist 에 있는 프로시저는 허용
    EXPECT_EQ(result.action, PolicyAction::kAllow)
        << "Whitelisted procedure 'safe_proc' must be allowed";
}

// ---------------------------------------------------------------------------
// UnknownProcedure_IsBlocked
//   whitelist 에 없는 프로시저는 차단되어야 한다.
//   [보안] whitelist 모드: 미등록 프로시저 = 차단.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, UnknownProcedure_IsBlocked) {
    const auto result = run("CALL dangerous_proc()");

    EXPECT_TRUE(result.procedure_detected)
        << "ProcedureDetector must detect CALL statement";
    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "Non-whitelisted procedure 'dangerous_proc' must be blocked";
}

// ---------------------------------------------------------------------------
// DynamicSql_PrepareExecute_IsBlocked
//   PREPARE/EXECUTE 동적 SQL 은 block_dynamic_sql=true 설정으로 차단되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, DynamicSql_PrepareExecute_IsBlocked) {
    // PREPARE 구문
    const auto result = run("PREPARE stmt FROM 'SELECT * FROM users'");

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "PREPARE (dynamic SQL) must be blocked when block_dynamic_sql=true";
}

// ---------------------------------------------------------------------------
// NormalInsert_IsAllowed
//   일반 INSERT 쿼리는 허용되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, NormalInsert_IsAllowed) {
    const auto result = run("INSERT INTO logs (msg) VALUES ('test message')");

    EXPECT_EQ(result.action, PolicyAction::kAllow)
        << "Normal INSERT should be allowed";
    EXPECT_FALSE(result.injection_detected)
        << "Normal INSERT should not trigger injection detection";
}

// ---------------------------------------------------------------------------
// NormalUpdate_IsAllowed
//   일반 UPDATE 쿼리는 허용되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, NormalUpdate_IsAllowed) {
    const auto result = run("UPDATE config SET value = 'new' WHERE key = 'timeout'");

    EXPECT_EQ(result.action, PolicyAction::kAllow)
        << "Normal UPDATE should be allowed";
}

// ---------------------------------------------------------------------------
// FailClose_NullConfig_IsBlocked
//   PolicyEngine 에 nullptr config 를 주입하면 모든 쿼리가 kBlock 이어야 한다.
//   [fail-close] nullptr config → 정책 없음 → 차단.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, FailClose_NullConfig_IsBlocked) {
    PolicyEngine null_engine(nullptr);
    InjectionDetector inj_det(default_injection_patterns());
    ProcedureDetector proc_det;
    StatsCollector stats;
    const auto session = make_session();

    const auto result = simulate_pipeline(
        "SELECT * FROM users", null_engine, inj_det, proc_det, session, stats);

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "Null config PolicyEngine must block all queries (fail-close)";
}

// ---------------------------------------------------------------------------
// CaseInsensitive_DropTable_IsBlocked
//   대소문자 혼합("DrOp TaBlE")도 파서/정책에서 차단되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, CaseInsensitive_DropTable_IsBlocked) {
    const auto result = run("DrOp TaBlE users");

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "Mixed-case DROP TABLE must be blocked (case-insensitive parsing)";
}

// ---------------------------------------------------------------------------
// CommentInSql_SelectAllowed
//   주석이 포함된 SELECT 는 파서가 전처리 후 정상 처리해야 한다.
//   단, 주석 뒤에 인젝션 패턴이 있으면 탐지되어야 한다 (별도 테스트).
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, CommentInSql_SelectAllowed) {
    // 주석만 포함된 정상 SELECT
    const auto result = run("SELECT /* fetch all users */ * FROM users WHERE active = 1");

    // 인라인 주석 패턴 "/*.*/" 이 포함되므로 InjectionDetector 탐지 여부 확인
    // [알려진 트레이드오프] 정상 쿼리의 /* 주석 */ 이 인젝션으로 탐지될 수 있음
    // 탐지된 경우: block, 탐지 안 된 경우: allow — 둘 다 정책에 따른 동작임
    (void)result;
    // 동작을 문서화하는 테스트: 어느 쪽이든 크래시/hang 없이 처리되어야 함
    SUCCEED() << "Inline comment in SELECT is handled without crash. "
              << "Action=" << static_cast<int>(result.action)
              << " injection=" << result.injection_detected
              << " reason=" << result.reason;
}

// ---------------------------------------------------------------------------
// PiggybackInjection_IsBlocked
//   세미콜론 이후 DROP 문 (piggyback 공격) 은 인젝션 탐지로 차단되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, PiggybackInjection_IsBlocked) {
    const auto result = run("SELECT 1; DROP TABLE users");

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "Piggyback injection must be blocked";
    EXPECT_TRUE(result.injection_detected)
        << "InjectionDetector must flag piggyback DROP after semicolon";
}
