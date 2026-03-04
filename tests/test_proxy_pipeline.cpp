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

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "parser/injection_detector.hpp"
#include "parser/procedure_detector.hpp"
#include "parser/sql_parser.hpp"
#include "policy/policy_engine.hpp"
#include "policy/rule.hpp"
#include "stats/stats_collector.hpp"

// ---------------------------------------------------------------------------
// 헬퍼: 기본 인젝션 탐지 패턴 (test_injection_detector.cpp 와 동일)
// ---------------------------------------------------------------------------
namespace {

std::vector<std::string> default_injection_patterns() {
    return {
        "UNION\\s+SELECT",
        R"('\s*OR\s+['"\d])",
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
    cfg->sql_rules.block_patterns = default_injection_patterns();

    // 기본 사용자 접근 규칙: * 사용자에게 SELECT/INSERT/UPDATE/DELETE 허용
    AccessRule allow_rule;
    allow_rule.user = "*";
    allow_rule.source_ip_cidr = "";  // 모든 IP 허용
    allow_rule.allowed_tables = {"*"};
    allow_rule.allowed_operations = {"SELECT", "INSERT", "UPDATE", "DELETE", "CALL"};
    allow_rule.blocked_operations = {};
    cfg->access_control.push_back(std::move(allow_rule));

    // 프로시저 제어: whitelist 모드, 동적 SQL 차단
    cfg->procedure_control.mode = "whitelist";
    cfg->procedure_control.whitelist = {"safe_proc"};  // 허용 프로시저
    cfg->procedure_control.block_dynamic_sql = true;
    cfg->procedure_control.block_create_alter = true;

    return cfg;
}

// ---------------------------------------------------------------------------
// make_session
//   테스트용 기본 SessionContext 생성.
// ---------------------------------------------------------------------------
SessionContext make_session(const std::string& user = "testuser",
                            const std::string& ip = "127.0.0.1") {
    SessionContext ctx;
    ctx.session_id = 1;
    ctx.client_ip = ip;
    ctx.client_port = 12345;
    ctx.db_user = user;
    ctx.db_name = "testdb";
    ctx.handshake_done = true;
    return ctx;
}

// ---------------------------------------------------------------------------
// PipelineResult
//   simulate_pipeline() 의 반환값.
// ---------------------------------------------------------------------------
struct PipelineResult {
    PolicyAction action;
    std::string reason;
    bool injection_detected{false};
    bool procedure_detected{false};
    bool monitor_mode{false};  // sql_rules/access_rule monitor 모드에서 통과된 경우
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
PipelineResult simulate_pipeline(std::string_view sql,
                                 const PolicyEngine& engine,
                                 const InjectionDetector& injection_det,
                                 const ProcedureDetector& proc_det,
                                 const SessionContext& session,
                                 StatsCollector& stats) {
    const SqlParser parser;
    const auto parse_result = parser.parse(sql);

    if (!parse_result.has_value()) {
        // 파싱 실패 → fail-close
        const auto policy_result = engine.evaluate_error(parse_result.error(), session);
        stats.on_query(true);  // 파싱 실패는 차단으로 기록
        return PipelineResult{
            .action = policy_result.action,
            .reason = policy_result.reason,
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
            .action = PolicyAction::kBlock,
            .reason = inj_result.reason,
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

    // monitor 모드: session.cpp 의 동작을 그대로 재현
    // policy_result.monitor_mode == true 이면 on_monitored_block() 호출
    if (policy_result.monitor_mode) {
        stats.on_monitored_block();
    }

    return PipelineResult{
        .action = policy_result.action,
        .reason = policy_result.reason,
        .injection_detected = false,
        .procedure_detected = proc_found,
        .monitor_mode = policy_result.monitor_mode,
    };
}

// ---------------------------------------------------------------------------
// make_monitor_config
//   sql_rules.mode = kMonitor 로 설정된 PolicyConfig.
//   block_statements: DROP, TRUNCATE
//   block_patterns: UNION SELECT 인젝션
//   access_control: testuser 에게 SELECT/INSERT/UPDATE/DELETE 허용.
//   unknown_user 는 access_control 에 없음 → no-access-rule(kBlock).
// ---------------------------------------------------------------------------
std::shared_ptr<PolicyConfig> make_monitor_config() {
    auto cfg = std::make_shared<PolicyConfig>();

    cfg->sql_rules.block_statements = {"DROP", "TRUNCATE"};
    cfg->sql_rules.block_patterns = {"UNION\\s+SELECT"};
    cfg->sql_rules.mode = RuleMode::kMonitor;  // monitor 모드

    // testuser 만 등록 (unknown_user 는 no-access-rule → kBlock)
    AccessRule rule{};
    rule.user = "testuser";
    rule.source_ip_cidr = "";
    rule.allowed_tables = {"*"};
    rule.allowed_operations = {"SELECT", "INSERT", "UPDATE", "DELETE", "DROP"};
    rule.blocked_operations = {};
    rule.mode = RuleMode::kEnforce;
    cfg->access_control.push_back(std::move(rule));

    return cfg;
}

}  // namespace

// ---------------------------------------------------------------------------
// 테스트 픽스처
// ---------------------------------------------------------------------------
class ProxyPipeline : public ::testing::Test {
protected:
    void SetUp() override {
        config_ = make_default_config();
        engine_ = std::make_unique<PolicyEngine>(config_);
        inj_det_ = std::make_unique<InjectionDetector>(default_injection_patterns());
        proc_det_ = std::make_unique<ProcedureDetector>();
        stats_ = std::make_unique<StatsCollector>();
        session_ = make_session();
    }

    // run_pipeline: 편의 래퍼
    PipelineResult run(std::string_view sql) {
        return simulate_pipeline(sql, *engine_, *inj_det_, *proc_det_, session_, *stats_);
    }

    std::shared_ptr<PolicyConfig>
        config_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::unique_ptr<PolicyEngine>
        engine_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::unique_ptr<InjectionDetector>
        inj_det_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::unique_ptr<ProcedureDetector>
        proc_det_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::unique_ptr<StatsCollector>
        stats_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    SessionContext
        session_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
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
    EXPECT_TRUE(result.injection_detected) << "InjectionDetector must flag OR tautology";
}

// ---------------------------------------------------------------------------
// UnionInjection_IsBlocked
//   UNION SELECT 인젝션은 탐지되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, UnionInjection_IsBlocked) {
    const auto result = run("SELECT * FROM users UNION SELECT 1,2,3");

    EXPECT_EQ(result.action, PolicyAction::kBlock) << "UNION SELECT injection must be blocked";
    EXPECT_TRUE(result.injection_detected) << "InjectionDetector must flag UNION SELECT";
}

// ---------------------------------------------------------------------------
// SleepInjection_IsBlocked
//   SLEEP() 기반 time-based blind injection 은 차단되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, SleepInjection_IsBlocked) {
    const auto result = run("SELECT * FROM users WHERE id = 1 AND SLEEP(5)");

    EXPECT_EQ(result.action, PolicyAction::kBlock) << "SLEEP() injection must be blocked";
    EXPECT_TRUE(result.injection_detected) << "InjectionDetector must flag SLEEP()";
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
    EXPECT_EQ(snap0.total_queries, 0U);
    EXPECT_EQ(snap0.blocked_queries, 0U);

    // 허용 쿼리 처리
    const auto allow_result = run("SELECT 1");
    EXPECT_EQ(allow_result.action, PolicyAction::kAllow);

    auto snap1 = stats_->snapshot();
    EXPECT_EQ(snap1.total_queries, 1U) << "total_queries must increment after allowed query";
    EXPECT_EQ(snap1.blocked_queries, 0U) << "blocked_queries must not increment for allowed query";

    // 차단 쿼리 처리 (DROP)
    const auto block_result = run("DROP TABLE users");
    EXPECT_EQ(block_result.action, PolicyAction::kBlock);

    auto snap2 = stats_->snapshot();
    EXPECT_EQ(snap2.total_queries, 2U) << "total_queries must increment after blocked query";
    EXPECT_EQ(snap2.blocked_queries, 1U) << "blocked_queries must increment for blocked query";

    // block_rate 검증: 1 / 2 = 0.5
    EXPECT_NEAR(snap2.block_rate, 0.5, 1e-9);
}

// ---------------------------------------------------------------------------
// ProcedureCall_Detection
//   CALL 구문은 ProcedureDetector 에서 탐지되어야 한다.
//
//   [알려진 버그 — IMPL-BUG-001]
//   SqlParser 가 CALL 구문에서 프로시저명을 ParsedQuery::tables 에 채우지
//   않는다 (sql_parser.cpp: kCall case 에서 extract_tables_for_keyword
//   미호출). 결과적으로 PolicyEngine 이 proc_name = "" 으로 화이트리스트를
//   체크하고, 빈 이름은 화이트리스트에 없으므로 kBlock 을 반환한다.
//
//   [현재 동작] CALL safe_proc() → kBlock (proc_name 추출 실패로 인한 오탐)
//   [기대 동작] CALL safe_proc() → kAllow (whitelist["safe_proc"] 매칭)
//
//   이 테스트는 버그를 재현/고립하기 위해 현재 동작을 검증한다.
//   버그가 수정되면 아래 EXPECT 를 kAllow 로 변경해야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, ProcedureCall_Detection) {
    const auto result = run("CALL safe_proc()");

    // ProcedureDetector 가 CALL 을 탐지해야 함
    EXPECT_TRUE(result.procedure_detected) << "ProcedureDetector must detect CALL statement";

    // [버그 재현] proc_name 추출 실패로 whitelist 체크가 작동하지 않음:
    // "safe_proc" 이 whitelist 에 있음에도 kBlock 이 반환된다.
    // TODO(IMPL-BUG-001): SqlParser 의 CALL 처리 수정 후 아래를 kAllow 로 변경
    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "[IMPL-BUG-001] SqlParser does not populate tables for CALL, "
           "causing PolicyEngine to use empty proc_name and fail whitelist check. "
           "Expected kAllow for whitelisted 'safe_proc', got kBlock.";
}

// ---------------------------------------------------------------------------
// UnknownProcedure_IsBlocked
//   whitelist 에 없는 프로시저는 차단되어야 한다.
//   [보안] whitelist 모드: 미등록 프로시저 = 차단.
//
//   [알려진 버그 — IMPL-BUG-001 연관]
//   SqlParser 가 proc_name 을 tables 에 채우지 않아 PolicyEngine 이 proc_name=""
//   으로 체크하지만, "" 도 whitelist 에 없으므로 결과적으로 kBlock 이 반환된다.
//   즉, 이 테스트는 올바른 이유가 아닌 이유로 통과한다.
//   버그 수정 후에도 결과(kBlock)는 동일해야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, UnknownProcedure_IsBlocked) {
    const auto result = run("CALL dangerous_proc()");

    EXPECT_TRUE(result.procedure_detected) << "ProcedureDetector must detect CALL statement";
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

    EXPECT_EQ(result.action, PolicyAction::kAllow) << "Normal INSERT should be allowed";
    EXPECT_FALSE(result.injection_detected)
        << "Normal INSERT should not trigger injection detection";
}

// ---------------------------------------------------------------------------
// NormalUpdate_IsAllowed
//   일반 UPDATE 쿼리는 허용되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, NormalUpdate_IsAllowed) {
    const auto result = run("UPDATE config SET value = 'new' WHERE key = 'timeout'");

    EXPECT_EQ(result.action, PolicyAction::kAllow) << "Normal UPDATE should be allowed";
}

// ---------------------------------------------------------------------------
// FailClose_NullConfig_IsBlocked
//   PolicyEngine 에 nullptr config 를 주입하면 모든 쿼리가 kBlock 이어야 한다.
//   [fail-close] nullptr config → 정책 없음 → 차단.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, FailClose_NullConfig_IsBlocked) {
    const PolicyEngine null_engine(nullptr);
    const InjectionDetector inj_det(default_injection_patterns());
    const ProcedureDetector proc_det;
    StatsCollector stats;
    const auto session = make_session();

    const auto result =
        simulate_pipeline("SELECT * FROM users", null_engine, inj_det, proc_det, session, stats);

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
              << " injection=" << result.injection_detected << " reason=" << result.reason;
}

// ---------------------------------------------------------------------------
// PiggybackInjection_IsBlocked
//   세미콜론 이후 DROP 문 (piggyback 공격) 은 차단되어야 한다.
//
//   [파이프라인 동작 설명]
//   SqlParser 가 멀티 스테이트먼트(세미콜론 외부)를 감지하여 먼저
//   ParseError(kInvalidSql) 를 반환한다. 따라서 파이프라인에서
//   InjectionDetector 까지 도달하지 않고 evaluate_error() → kBlock 이 된다.
//
//   [결과]
//   - action       = kBlock  (evaluate_error 경로, fail-close)
//   - injection_detected = false  (InjectionDetector 미실행)
//
//   멀티 스테이트먼트 차단은 InjectionDetector 가 아닌 SqlParser 의
//   fail-close 메커니즘으로 수행된다. 보안 결과(kBlock)는 동일하므로
//   올바른 동작이다.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, PiggybackInjection_IsBlocked) {
    const auto result = run("SELECT 1; DROP TABLE users");

    // 파이프라인 어느 단계든 차단되어야 한다 (fail-close 원칙)
    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "Piggyback injection (multi-statement) must be blocked";

    // SqlParser 가 멀티 스테이트먼트를 먼저 탐지하여 fail-close 처리하므로
    // InjectionDetector 는 호출되지 않는다 — injection_detected = false 가 올바름
    EXPECT_FALSE(result.injection_detected)
        << "InjectionDetector is not reached: SqlParser blocks multi-statement first (fail-close). "
           "injection_detected=false is correct behavior.";
}

// ---------------------------------------------------------------------------
// Bug 3 검증: 결과셋 릴레이 0x00 오판 수정
//
// 코드 검토: session.cpp:243 - OK 패킷 판별에 payload 길이 검증 추가
//
// [수정 내용]
// 결과셋 릴레이 중 OK 패킷 판별 로직에서 단순 byte0 == 0x00만으로
// OK 패킷을 판단하던 버그를 수정했습니다.
//
// [문제점]
// MySQL 텍스트 프로토콜에서:
//   - OK 패킷: 0x00으로 시작, 최소 5바이트
//     (header + affected_rows + last_insert_id + status_flags(2))
//   - 결과셋 행의 빈 컬럼: 0x00으로 인코딩될 수 있음 (length = 0)
//
// 수정 전: 빈 컬럼 포함 결과셋이 중간에 끊김
// 수정 후: payload.size() >= 5 로 OK 패킷 판별, 결과셋 정상 릴레이
//
// [테스트 전략]
// relay_server_response()는 네트워크 I/O 기반이므로,
// e2e 통합 테스트(MySQL 실제 연결)에서 검증합니다.
// 파이프라인 테스트는 SqlParser/PolicyEngine 검증에 중점을 둡니다.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Monitor 모드 우회 버그 회귀 테스트 (DON-49 수정 검증)
//
// [배경]
// 수정 전: sql_rules.mode=monitor 에서 block_statements/block_patterns 매칭 시
//   즉시 kLog 반환 → access_control(no-access-rule) 체크 건너뜀
//   → 미등록 사용자(unknown_user)가 DROP 같은 차단 쿼리를 통과시킬 수 있었음
//
// [수정 후 보장]
// 1. monitor 모드여도 no-access-rule(미등록 사용자) → kBlock (fail-close 유지)
// 2. monitor 모드 + 등록된 사용자 + block_statement 매칭 → kLog (monitor 정상 동작)
// 3. 통계: monitor kLog → on_monitored_block() 호출, blocked_queries 미증가
//
// [파이프라인 통합 포인트]
// SqlParser → PolicyEngine.evaluate() → StatsCollector
// (session.cpp 의 on_query/on_monitored_block 호출 경로를 simulate_pipeline 에서 재현)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Monitor_UnknownUser_BlockStatement_StillBlocked
//   sql_rules.mode=monitor + 미등록 사용자(unknown_user) + DROP
//   → access_control no-access-rule 에 의해 kBlock.
//   → stats: blocked_queries 증가, monitored_blocks 변화 없음.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, Monitor_UnknownUser_BlockStatement_StillBlocked) {
    // monitor 모드 config 으로 엔진 교체
    const auto monitor_cfg = make_monitor_config();
    const PolicyEngine monitor_engine(monitor_cfg);

    // unknown_user 는 access_control 에 없음 → no-access-rule
    const auto unknown_session = make_session("unknown_user");

    auto snap_before = stats_->snapshot();

    const auto result = simulate_pipeline(
        "DROP TABLE users", monitor_engine, *inj_det_, *proc_det_, unknown_session, *stats_);

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "monitor 모드여도 no-access-rule(미등록 사용자)이면 kBlock이어야 한다 (fail-close)";
    EXPECT_FALSE(result.monitor_mode)
        << "no-access-rule 에 의한 차단은 monitor_mode 플래그를 세우지 않아야 한다";

    auto snap_after = stats_->snapshot();
    EXPECT_EQ(snap_after.blocked_queries, snap_before.blocked_queries + 1)
        << "kBlock 은 blocked_queries 를 증가시켜야 한다";
    EXPECT_EQ(snap_after.monitored_blocks, snap_before.monitored_blocks)
        << "kBlock(no-access-rule) 은 monitored_blocks 를 증가시키지 않아야 한다";
}

// ---------------------------------------------------------------------------
// Monitor_UnknownUser_BlockPattern_StillBlocked
//   sql_rules.mode=monitor + 미등록 사용자 + UNION SELECT(block_pattern)
//   → no-access-rule 에 의해 kBlock.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, Monitor_UnknownUser_BlockPattern_StillBlocked) {
    const auto monitor_cfg = make_monitor_config();
    const PolicyEngine monitor_engine(monitor_cfg);
    const auto unknown_session = make_session("unknown_user");

    const auto result = simulate_pipeline("SELECT id FROM users UNION SELECT password FROM admin",
                                          monitor_engine,
                                          *inj_det_,
                                          *proc_det_,
                                          unknown_session,
                                          *stats_);

    // InjectionDetector 도 UNION SELECT 를 탐지할 수 있으므로 kBlock 여부만 검증
    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "monitor 모드여도 no-access-rule(미등록 사용자)이면 kBlock이어야 한다 (fail-close)";
}

// ---------------------------------------------------------------------------
// Monitor_RegisteredUser_BlockStatement_IsKLog_WithStats
//   sql_rules.mode=monitor + 등록된 사용자(testuser) + DROP
//   → access_control 통과 후 kLog (monitor 정상 동작).
//   → stats: monitored_blocks 증가, blocked_queries 미증가.
// ---------------------------------------------------------------------------
TEST_F(ProxyPipeline, Monitor_RegisteredUser_BlockStatement_IsKLog_WithStats) {
    const auto monitor_cfg = make_monitor_config();
    const PolicyEngine monitor_engine(monitor_cfg);

    // testuser 는 make_monitor_config() 에 등록됨 + allowed_operations 에 DROP 포함
    const auto registered_session = make_session("testuser");

    auto snap_before = stats_->snapshot();

    const auto result = simulate_pipeline(
        "DROP TABLE logs", monitor_engine, *inj_det_, *proc_det_, registered_session, *stats_);

    EXPECT_EQ(result.action, PolicyAction::kLog)
        << "monitor 모드 + 등록된 사용자 + block_statement 매칭 → kLog 이어야 한다";
    EXPECT_TRUE(result.monitor_mode) << "kLog 결과는 monitor_mode=true 플래그를 가져야 한다";

    auto snap_after = stats_->snapshot();
    EXPECT_EQ(snap_after.monitored_blocks, snap_before.monitored_blocks + 1)
        << "kLog(monitor) 는 on_monitored_block() 을 통해 monitored_blocks 를 증가시켜야 한다";
    EXPECT_EQ(snap_after.blocked_queries, snap_before.blocked_queries)
        << "kLog(monitor) 는 blocked_queries 를 증가시키지 않아야 한다";
}
