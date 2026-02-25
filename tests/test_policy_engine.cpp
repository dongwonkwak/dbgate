// ---------------------------------------------------------------------------
// test_policy_engine.cpp
//
// PolicyEngine 단위 테스트.
//
// [테스트 범위]
// - Fail-close 동작: nullptr config, unknown command, parse error, 빈 access_control
// - SQL 구문 차단 (block_statements)
// - SQL 패턴 차단 (block_patterns, regex)
// - 사용자/IP 접근 제어 (access_control, CIDR 매칭)
// - CIDR 경계값: /32, /0, /16, 잘못된 형식 (prefix>32, 비숫자, 음수, 빈값)
// - 테이블 접근 제어 (allowed_tables)
// - 차단/허용 오퍼레이션 (blocked/allowed_operations)
// - blocked_operations vs allowed_operations 우선순위
// - 시간대 제한 (time_restriction) — UTC 기반 / 자정 초과 범위 / 잘못된 형식
// - 동적 SQL 차단/허용 (block_dynamic_sql: true/false)
// - 프로시저 제어 (whitelist/blacklist, dynamic SQL, create/alter)
// - 스키마 접근 차단 (INFORMATION_SCHEMA)
// - Hot Reload (reload() 호출, nullptr 전환, 다중 reload)
// - PolicyLoader: 잘못된 CIDR/시간 형식 YAML 로딩, config/policy.yaml 실제 로딩
//
// [오탐/미탐 트레이드오프]
// - block_patterns 의 regex 는 ORM 생성 쿼리에서 false positive 발생 가능.
// - CIDR 매칭 오류 시 fail-close (false positive 증가).
// - 시간대 테스트는 UTC 기반으로 수행하여 환경 의존성을 최소화.
//
// [알려진 한계]
// - 시간대 테스트는 실행 시각에 따라 결과가 달라질 수 있다.
//   UTC "00:00-23:59" 범위로 항상 허용되는 케이스만 테스트.
// - IPv6 CIDR 매칭 미지원 (IPv4 전용).
// ---------------------------------------------------------------------------

#include "policy/policy_engine.hpp"
#include "policy/policy_loader.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 테스트 헬퍼: 기본 PolicyConfig 생성
// ---------------------------------------------------------------------------
namespace {

SessionContext make_session(
    const std::string& user = "testuser",
    const std::string& ip   = "192.168.1.100",
    std::uint64_t      sid  = 1
) {
    SessionContext ctx{};
    ctx.session_id   = sid;
    ctx.client_ip    = ip;
    ctx.client_port  = 3306;
    ctx.db_user      = user;
    ctx.db_name      = "testdb";
    ctx.handshake_done = true;
    return ctx;
}

ParsedQuery make_query(
    SqlCommand               cmd     = SqlCommand::kSelect,
    std::vector<std::string> tables  = {"users"},
    const std::string&       raw_sql = "SELECT * FROM users"
) {
    ParsedQuery q{};
    q.command         = cmd;
    q.tables          = std::move(tables);
    q.raw_sql         = raw_sql;
    q.has_where_clause = false;
    return q;
}

// 최소 유효 PolicyConfig (access_control에 testuser 허용 룰 포함)
std::shared_ptr<PolicyConfig> make_basic_config() {
    auto cfg = std::make_shared<PolicyConfig>();

    // sql_rules: block_statements, block_patterns 설정
    cfg->sql_rules.block_statements = {"DROP", "TRUNCATE"};
    cfg->sql_rules.block_patterns   = {"UNION\\s+SELECT", "SLEEP\\s*\\("};

    // access_control: testuser → 모든 테이블, SELECT/INSERT/UPDATE 허용
    AccessRule rule{};
    rule.user             = "testuser";
    rule.source_ip_cidr   = "192.168.1.0/24";
    rule.allowed_tables   = {"users", "orders", "products"};
    rule.allowed_operations = {"SELECT", "INSERT", "UPDATE"};
    cfg->access_control.push_back(rule);

    // procedure_control: whitelist 모드, 특정 프로시저만 허용
    cfg->procedure_control.mode              = "whitelist";
    cfg->procedure_control.whitelist         = {"sp_get_user"};
    cfg->procedure_control.block_dynamic_sql = true;
    cfg->procedure_control.block_create_alter = true;

    // data_protection: 스키마 접근 차단
    cfg->data_protection.block_schema_access = true;
    cfg->data_protection.max_result_rows     = 10000;

    return cfg;
}

}  // namespace

// ===========================================================================
// Step 1: Fail-close — nullptr config
// ===========================================================================

TEST(PolicyEngine, NullptrConfig_BlocksAllQueries) {
    // nullptr config 로 생성 → 모든 쿼리 차단 (fail-close)
    PolicyEngine engine(nullptr);
    const auto query   = make_query();
    const auto session = make_session();
    const auto result  = engine.evaluate(query, session);

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "no-config");
}

// ===========================================================================
// Step 2: Fail-close — unknown command
// ===========================================================================

TEST(PolicyEngine, UnknownCommand_BlocksQuery) {
    PolicyEngine engine(make_basic_config());
    const auto query   = make_query(SqlCommand::kUnknown, {}, "???");
    const auto session = make_session();
    const auto result  = engine.evaluate(query, session);

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "unknown-command");
}

// ===========================================================================
// Step 3: SQL 구문 차단 (block_statements)
// ===========================================================================

TEST(PolicyEngine, BlockStatement_Drop) {
    PolicyEngine engine(make_basic_config());
    const auto query = make_query(SqlCommand::kDrop, {"users"}, "DROP TABLE users");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "block-statement");
    EXPECT_NE(result.reason.find("DROP"), std::string::npos);
}

TEST(PolicyEngine, BlockStatement_Truncate) {
    PolicyEngine engine(make_basic_config());
    const auto query = make_query(SqlCommand::kTruncate, {"orders"}, "TRUNCATE TABLE orders");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "block-statement");
}

TEST(PolicyEngine, BlockStatement_CaseInsensitive) {
    // block_statements 비교는 대소문자 무관
    auto cfg = make_basic_config();
    cfg->sql_rules.block_statements = {"drop"};  // 소문자로 설정
    PolicyEngine engine(cfg);

    // SqlCommand::kDrop 은 "DROP" 으로 변환되므로 iequals("DROP", "drop") = true
    const auto query = make_query(SqlCommand::kDrop, {}, "DROP TABLE x");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "block-statement");
}

TEST(PolicyEngine, NoBlockStatement_SelectAllowed) {
    // SELECT 는 block_statements 에 없으므로 차단되지 않음
    PolicyEngine engine(make_basic_config());
    const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT * FROM users");
    const auto result = engine.evaluate(query, make_session());

    // SELECT 는 block_statements 통과 → 이후 단계에서 허용
    EXPECT_NE(result.matched_rule, "block-statement");
}

// ===========================================================================
// Step 4: SQL 패턴 차단 (block_patterns)
// ===========================================================================

TEST(PolicyEngine, BlockPattern_UnionSelect) {
    PolicyEngine engine(make_basic_config());
    const auto query = make_query(
        SqlCommand::kSelect, {"users"},
        "SELECT * FROM users UNION SELECT 1,2,3"
    );
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "block-pattern");
}

TEST(PolicyEngine, BlockPattern_Sleep) {
    PolicyEngine engine(make_basic_config());
    const auto query = make_query(
        SqlCommand::kSelect, {},
        "SELECT SLEEP(5)"
    );
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "block-pattern");
}

TEST(PolicyEngine, BlockPattern_CaseInsensitive) {
    // block_patterns 도 대소문자 무관 (icase 플래그)
    PolicyEngine engine(make_basic_config());
    const auto query = make_query(
        SqlCommand::kSelect, {},
        "select * from t union select 1"
    );
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "block-pattern");
}

TEST(PolicyEngine, BlockPattern_InvalidRegex_Skipped) {
    // 잘못된 regex 패턴은 건너뜀 (나머지 유효 패턴은 동작)
    auto cfg = make_basic_config();
    cfg->sql_rules.block_patterns = {
        "[invalid_regex",      // 잘못된 패턴: 건너뜀
        "UNION\\s+SELECT",     // 유효한 패턴: 동작
    };
    PolicyEngine engine(cfg);

    const auto query = make_query(
        SqlCommand::kSelect, {},
        "SELECT 1 UNION SELECT 2"
    );
    const auto result = engine.evaluate(query, make_session());

    // 유효한 패턴으로 탐지
    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "block-pattern");
}

// ===========================================================================
// Step 5: 사용자/IP 접근 제어
// ===========================================================================

TEST(PolicyEngine, NoMatchingRule_Blocks) {
    PolicyEngine engine(make_basic_config());
    // unknown_user 는 access_control 에 없음
    const auto session = make_session("unknown_user", "10.0.0.1");
    const auto query   = make_query();
    const auto result  = engine.evaluate(query, session);

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "no-access-rule");
}

TEST(PolicyEngine, WildcardUser_Matches) {
    auto cfg = make_basic_config();
    // 와일드카드 룰 추가 (SELECT 만 허용, 모든 IP)
    AccessRule wildcard_rule{};
    wildcard_rule.user             = "*";
    wildcard_rule.source_ip_cidr   = "";
    wildcard_rule.allowed_tables   = {"*"};
    wildcard_rule.allowed_operations = {"SELECT"};
    cfg->access_control.push_back(wildcard_rule);

    PolicyEngine engine(cfg);
    // any_user 는 와일드카드 룰에 매칭됨
    const auto session = make_session("any_user", "1.2.3.4");
    const auto query   = make_query(SqlCommand::kSelect, {"users"}, "SELECT * FROM users");
    const auto result  = engine.evaluate(query, session);

    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

TEST(PolicyEngine, IpCidr_OutOfRange_Blocks) {
    PolicyEngine engine(make_basic_config());
    // testuser 이지만 IP가 192.168.1.0/24 범위 밖
    const auto session = make_session("testuser", "10.0.0.1");
    const auto query   = make_query();
    const auto result  = engine.evaluate(query, session);

    // CIDR 불일치 → 룰 매칭 실패 → kBlock
    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "no-access-rule");
}

TEST(PolicyEngine, IpCidr_InRange_Matches) {
    PolicyEngine engine(make_basic_config());
    // 192.168.1.50 는 192.168.1.0/24 범위 내
    const auto session = make_session("testuser", "192.168.1.50");
    const auto query   = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
    const auto result  = engine.evaluate(query, session);

    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

TEST(PolicyEngine, EmptySourceIp_AllowsAllIps) {
    auto cfg = make_basic_config();
    cfg->access_control[0].source_ip_cidr = "";  // 빈 문자열 = 모든 IP 허용
    PolicyEngine engine(cfg);

    const auto session = make_session("testuser", "203.0.113.1");
    const auto query   = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
    const auto result  = engine.evaluate(query, session);

    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

// ===========================================================================
// Step 6: 차단 오퍼레이션 (blocked_operations)
// ===========================================================================

TEST(PolicyEngine, BlockedOperation_Denied) {
    auto cfg = make_basic_config();
    cfg->access_control[0].blocked_operations = {"DELETE"};
    PolicyEngine engine(cfg);

    const auto query  = make_query(SqlCommand::kDelete, {"users"}, "DELETE FROM users WHERE id=1");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "blocked-operation");
}

TEST(PolicyEngine, BlockedOperation_DoesNotAffectAllowed) {
    // blocked_operations 에 없는 오퍼레이션은 영향 없음
    auto cfg = make_basic_config();
    cfg->access_control[0].blocked_operations = {"DELETE"};
    PolicyEngine engine(cfg);

    const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_NE(result.matched_rule, "blocked-operation");
}

// ===========================================================================
// Step 7: 시간대 제한 (time_restriction)
// UTC 기반 — 항상 허용되는 광범위한 범위("00:00-23:59")로 테스트
// ===========================================================================

TEST(PolicyEngine, TimeRestriction_AlwaysAllow_UTC) {
    auto cfg = make_basic_config();
    TimeRestriction tr{};
    tr.allow_range = "00:00-23:59";
    tr.timezone    = "UTC";
    cfg->access_control[0].time_restriction = tr;
    PolicyEngine engine(cfg);

    const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
    const auto result = engine.evaluate(query, make_session());

    // 00:00-23:59 UTC 범위 = 항상 허용
    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

TEST(PolicyEngine, TimeRestriction_InvalidRange_Blocks) {
    // 잘못된 allow_range → fail-close (차단)
    auto cfg = make_basic_config();
    TimeRestriction tr{};
    tr.allow_range = "invalid-range";
    tr.timezone    = "UTC";
    cfg->access_control[0].time_restriction = tr;
    PolicyEngine engine(cfg);

    const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "time-restriction");
}

TEST(PolicyEngine, TimeRestriction_NoRestriction_Allows) {
    // time_restriction = nullopt → 24시간 허용
    auto cfg = make_basic_config();
    cfg->access_control[0].time_restriction = std::nullopt;
    PolicyEngine engine(cfg);

    const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

// ===========================================================================
// Step 8: 테이블 접근 제어 (allowed_tables)
// ===========================================================================

TEST(PolicyEngine, TableDenied_NotInAllowedList) {
    PolicyEngine engine(make_basic_config());
    // "salary" 테이블은 allowed_tables 에 없음
    const auto query = make_query(SqlCommand::kSelect, {"salary"}, "SELECT * FROM salary");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "table-denied");
    EXPECT_NE(result.reason.find("salary"), std::string::npos);
}

TEST(PolicyEngine, TableAllowed_InAllowedList) {
    PolicyEngine engine(make_basic_config());
    const auto query  = make_query(SqlCommand::kSelect, {"orders"}, "SELECT * FROM orders");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

TEST(PolicyEngine, TableAllowed_WildcardPermitsAll) {
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_tables = {"*"};
    PolicyEngine engine(cfg);

    const auto query  = make_query(SqlCommand::kSelect, {"salary"}, "SELECT * FROM salary");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

TEST(PolicyEngine, TableAllowed_EmptyTables_Skips) {
    // query.tables 가 비어있으면 테이블 체크 건너뜀 (허용)
    PolicyEngine engine(make_basic_config());
    const auto query = make_query(SqlCommand::kSelect, {}, "SELECT 1");
    const auto result = engine.evaluate(query, make_session());

    // 테이블 체크 건너뜀 → allowed_operations 통과 → kAllow
    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

TEST(PolicyEngine, TableAllowed_CaseInsensitive) {
    PolicyEngine engine(make_basic_config());
    // "USERS" 는 대소문자 무관 비교로 "users" 와 매칭
    const auto query  = make_query(SqlCommand::kSelect, {"USERS"}, "SELECT * FROM USERS");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

// ===========================================================================
// Step 9: 허용 오퍼레이션 (allowed_operations)
// ===========================================================================

TEST(PolicyEngine, OperationDenied_NotInAllowedList) {
    PolicyEngine engine(make_basic_config());
    // DELETE 는 allowed_operations = {SELECT, INSERT, UPDATE} 에 없음
    const auto query  = make_query(SqlCommand::kDelete, {"users"}, "DELETE FROM users WHERE 1=0");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "operation-denied");
}

TEST(PolicyEngine, OperationAllowed_InList) {
    PolicyEngine engine(make_basic_config());
    const auto query  = make_query(SqlCommand::kInsert, {"users"}, "INSERT INTO users VALUES(1,'a')");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

TEST(PolicyEngine, OperationAllowed_WildcardPermitsAll) {
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_operations = {"*"};
    PolicyEngine engine(cfg);

    // DELETE 도 와일드카드에 의해 허용됨
    const auto query  = make_query(SqlCommand::kDelete, {"users"}, "DELETE FROM users WHERE id=1");
    const auto result = engine.evaluate(query, make_session());

    // block_statements 에 DELETE 없고, allowed_operations = * → 허용
    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

TEST(PolicyEngine, OperationAllowed_EmptyAllowedOps_PermitsAll) {
    // allowed_operations 가 비어있으면 제한 없음
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_operations = {};
    PolicyEngine engine(cfg);

    const auto query  = make_query(SqlCommand::kDelete, {"users"}, "DELETE FROM users WHERE id=1");
    const auto result = engine.evaluate(query, make_session());

    // allowed_operations 비어있음 → 오퍼레이션 체크 건너뜀 → 허용
    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

// ===========================================================================
// Step 10: 프로시저 제어
// ===========================================================================

TEST(PolicyEngine, Procedure_PrepareBlocked_DynamicSql) {
    // PREPARE 는 allowed_operations 에 포함시켜 Step 9 통과 후
    // Step 10 에서 block_dynamic_sql 로 차단됨을 검증한다.
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_operations = {"SELECT", "INSERT", "UPDATE", "PREPARE", "EXECUTE"};
    PolicyEngine engine(cfg);

    // block_dynamic_sql = true → PREPARE 차단
    const auto query = make_query(SqlCommand::kPrepare, {}, "PREPARE stmt FROM 'SELECT 1'");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "procedure-dynamic-sql");
}

TEST(PolicyEngine, Procedure_ExecuteBlocked_DynamicSql) {
    // EXECUTE 는 allowed_operations 에 포함시켜 Step 9 통과 후
    // Step 10 에서 block_dynamic_sql 로 차단됨을 검증한다.
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_operations = {"SELECT", "INSERT", "UPDATE", "PREPARE", "EXECUTE"};
    PolicyEngine engine(cfg);

    // block_dynamic_sql = true → EXECUTE 차단
    const auto query = make_query(SqlCommand::kExecute, {}, "EXECUTE stmt");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "procedure-dynamic-sql");
}

TEST(PolicyEngine, Procedure_Call_WhitelistAllowed) {
    // CALL 은 allowed_operations 에 포함시켜 Step 9 통과 후
    // Step 10 에서 whitelist 로 허용됨을 검증한다.
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_operations = {"SELECT", "INSERT", "UPDATE", "CALL"};
    cfg->access_control[0].allowed_tables     = {"*"};  // 프로시저명 테이블 체크 우회
    PolicyEngine engine(cfg);

    // sp_get_user 는 whitelist 에 있음 → 허용
    // 프로시저명은 tables 의 첫 번째 요소
    const auto query = make_query(SqlCommand::kCall, {"sp_get_user"}, "CALL sp_get_user(1)");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

TEST(PolicyEngine, Procedure_Call_WhitelistBlocked) {
    // CALL 은 allowed_operations 에 포함시켜 Step 9 통과 후
    // Step 10 에서 whitelist 로 차단됨을 검증한다.
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_operations = {"SELECT", "INSERT", "UPDATE", "CALL"};
    cfg->access_control[0].allowed_tables     = {"*"};
    PolicyEngine engine(cfg);

    // sp_admin 는 whitelist 에 없음 → 차단
    const auto query = make_query(SqlCommand::kCall, {"sp_admin"}, "CALL sp_admin()");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "procedure-whitelist");
}

TEST(PolicyEngine, Procedure_Call_BlacklistMode_Blocked) {
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_operations = {"SELECT", "INSERT", "UPDATE", "CALL"};
    cfg->access_control[0].allowed_tables     = {"*"};
    cfg->procedure_control.mode      = "blacklist";
    cfg->procedure_control.whitelist = {"sp_dangerous"};  // 블랙리스트로 재사용
    PolicyEngine engine(cfg);

    // sp_dangerous 는 blacklist 에 있으므로 차단
    const auto query = make_query(SqlCommand::kCall, {"sp_dangerous"}, "CALL sp_dangerous()");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "procedure-blacklist");
}

TEST(PolicyEngine, Procedure_Call_BlacklistMode_Allowed) {
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_operations = {"SELECT", "INSERT", "UPDATE", "CALL"};
    cfg->access_control[0].allowed_tables     = {"*"};
    cfg->procedure_control.mode      = "blacklist";
    cfg->procedure_control.whitelist = {"sp_dangerous"};
    PolicyEngine engine(cfg);

    // sp_safe 는 blacklist 에 없으므로 허용
    const auto query = make_query(SqlCommand::kCall, {"sp_safe"}, "CALL sp_safe()");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

TEST(PolicyEngine, Procedure_CreateBlocked) {
    // CREATE 는 allowed_operations 에 포함시켜 Step 9 통과 후
    // Step 10 에서 block_create_alter 로 차단됨을 검증한다.
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_operations = {"*"};  // 모든 오퍼레이션 허용
    PolicyEngine engine(cfg);

    // block_create_alter = true → CREATE 차단
    const auto query = make_query(SqlCommand::kCreate, {}, "CREATE PROCEDURE sp_new() BEGIN END");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "procedure-create-alter");
}

TEST(PolicyEngine, Procedure_AlterBlocked) {
    // ALTER 는 allowed_operations 에 포함시켜 Step 9 통과 후
    // Step 10 에서 block_create_alter 로 차단됨을 검증한다.
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_operations = {"*"};  // 모든 오퍼레이션 허용
    PolicyEngine engine(cfg);

    // block_create_alter = true → ALTER 차단
    const auto query = make_query(SqlCommand::kAlter, {}, "ALTER TABLE users ADD col INT");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "procedure-create-alter");
}

TEST(PolicyEngine, Procedure_CreateAllowed_WhenBlockCreateAlterFalse) {
    auto cfg = make_basic_config();
    cfg->procedure_control.block_create_alter = false;
    // allowed_operations 에 CREATE 추가
    cfg->access_control[0].allowed_operations = {"*"};
    PolicyEngine engine(cfg);

    const auto query = make_query(SqlCommand::kCreate, {"users"}, "CREATE TABLE new_t (id INT)");
    const auto result = engine.evaluate(query, make_session());

    // block_create_alter = false → CREATE 허용
    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

// ===========================================================================
// Step 11: 스키마 접근 차단
// ===========================================================================

TEST(PolicyEngine, SchemaAccess_InformationSchema_Blocked) {
    // allowed_tables = {"*"} 로 설정하여 Step 8 (테이블 접근 제어) 를 통과시킨 후
    // Step 11 (스키마 접근 차단) 에서 차단됨을 검증한다.
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_tables = {"*"};
    PolicyEngine engine(cfg);

    const auto query = make_query(
        SqlCommand::kSelect, {"information_schema"},
        "SELECT * FROM information_schema.tables"
    );
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "schema-access");
}

TEST(PolicyEngine, SchemaAccess_Mysql_Blocked) {
    // allowed_tables = {"*"} 로 설정하여 Step 8 통과 후 Step 11 에서 차단됨을 검증한다.
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_tables = {"*"};
    PolicyEngine engine(cfg);

    const auto query = make_query(
        SqlCommand::kSelect, {"mysql"},
        "SELECT * FROM mysql.user"
    );
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "schema-access");
}

TEST(PolicyEngine, SchemaAccess_PerformanceSchema_Blocked) {
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_tables = {"*"};  // 테이블 제한 없앰
    PolicyEngine engine(cfg);

    const auto query = make_query(
        SqlCommand::kSelect, {"performance_schema"},
        "SELECT * FROM performance_schema.events_statements_summary_global_by_event_name"
    );
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "schema-access");
}

TEST(PolicyEngine, SchemaAccess_CaseInsensitive_Blocked) {
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_tables = {"*"};
    PolicyEngine engine(cfg);

    // 대문자로 써도 차단
    const auto query = make_query(
        SqlCommand::kSelect, {"INFORMATION_SCHEMA"},
        "SELECT * FROM INFORMATION_SCHEMA.TABLES"
    );
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "schema-access");
}

TEST(PolicyEngine, SchemaAccess_Disabled_Allows) {
    auto cfg = make_basic_config();
    cfg->data_protection.block_schema_access = false;
    cfg->access_control[0].allowed_tables    = {"*"};
    PolicyEngine engine(cfg);

    const auto query = make_query(
        SqlCommand::kSelect, {"information_schema"},
        "SELECT * FROM information_schema.tables"
    );
    const auto result = engine.evaluate(query, make_session());

    // block_schema_access = false → 허용
    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

// ===========================================================================
// evaluate_error: Fail-close 보장
// ===========================================================================

TEST(PolicyEngine, EvaluateError_AlwaysBlocks) {
    PolicyEngine engine(make_basic_config());
    ParseError err{};
    err.code    = ParseErrorCode::kInvalidSql;
    err.message = "syntax error near 'SELECCT'";

    const auto session = make_session();
    const auto result  = engine.evaluate_error(err, session);

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "parse-error");
    EXPECT_NE(result.reason.find("syntax error"), std::string::npos);
}

TEST(PolicyEngine, EvaluateError_NullptrConfig_StillBlocks) {
    // nullptr config 에서도 evaluate_error 는 kBlock 반환
    PolicyEngine engine(nullptr);
    ParseError err{};
    err.code    = ParseErrorCode::kMalformedPacket;
    err.message = "malformed packet";

    const auto result = engine.evaluate_error(err, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "parse-error");
}

// ===========================================================================
// Hot Reload: reload() 호출 후 새 정책 적용
// ===========================================================================

TEST(PolicyEngine, Reload_NewConfigApplied) {
    auto initial_cfg = make_basic_config();
    PolicyEngine engine(initial_cfg);

    // 초기 상태: testuser 허용
    {
        const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
        const auto result = engine.evaluate(query, make_session());
        EXPECT_EQ(result.action, PolicyAction::kAllow);
    }

    // 새 config: 모든 SELECT 차단
    auto new_cfg = std::make_shared<PolicyConfig>();
    new_cfg->sql_rules.block_statements = {"SELECT"};
    new_cfg->sql_rules.block_patterns   = {"UNION\\s+SELECT"};
    // access_control 은 비워둠 (결과적으로 no-access-rule)

    engine.reload(new_cfg);

    // 재로드 후: SELECT 차단
    {
        const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
        const auto result = engine.evaluate(query, make_session());
        EXPECT_EQ(result.action, PolicyAction::kBlock);
    }
}

TEST(PolicyEngine, Reload_NullptrConfig_BlocksAll) {
    PolicyEngine engine(make_basic_config());

    // 초기 상태: 정상 동작
    {
        const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
        const auto result = engine.evaluate(query, make_session());
        EXPECT_EQ(result.action, PolicyAction::kAllow);
    }

    // nullptr 로 reload → 모든 쿼리 차단
    engine.reload(nullptr);

    {
        const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
        const auto result = engine.evaluate(query, make_session());
        EXPECT_EQ(result.action, PolicyAction::kBlock);
        EXPECT_EQ(result.matched_rule, "no-config");
    }
}

// ===========================================================================
// PolicyLoader 기본 동작
// ===========================================================================

TEST(PolicyLoader, LoadNonExistentFile_ReturnsError) {
    const auto result = PolicyLoader::load("/nonexistent/path/policy.yaml");
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().empty());
}

TEST(PolicyLoader, LoadValidFile_Succeeds) {
    // 임시 유효 YAML 파일 생성
    const std::string tmp_path = "/tmp/test_policy_valid.yaml";
    {
        std::FILE* f = std::fopen(tmp_path.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs(R"(
global:
  log_level: info
  log_format: json
  max_connections: 100
  connection_timeout: 10s

access_control:
  - user: "admin"
    source_ip: ""
    allowed_tables: ["*"]
    allowed_operations: ["*"]

sql_rules:
  block_statements:
    - DROP
  block_patterns:
    - "UNION\\s+SELECT"

procedure_control:
  mode: "whitelist"
  whitelist: []
  block_dynamic_sql: true
  block_create_alter: true

data_protection:
  max_result_rows: 1000
  block_schema_access: true
)", f);
        std::fclose(f);
    }

    const auto result = PolicyLoader::load(tmp_path);
    ASSERT_TRUE(result.has_value()) << "Expected success but got error: " << result.error();

    const auto& cfg = result.value();
    EXPECT_EQ(cfg.global.log_level, "info");
    EXPECT_EQ(cfg.global.connection_timeout_sec, 10u);
    EXPECT_EQ(cfg.global.max_connections, 100u);
    EXPECT_EQ(cfg.access_control.size(), 1u);
    EXPECT_EQ(cfg.sql_rules.block_statements.size(), 1u);
    EXPECT_EQ(cfg.sql_rules.block_patterns.size(), 1u);
    EXPECT_TRUE(cfg.procedure_control.block_dynamic_sql);
    EXPECT_TRUE(cfg.data_protection.block_schema_access);

    std::remove(tmp_path.c_str());
}

TEST(PolicyLoader, LoadFile_EmptyBlockPatterns_ReturnsError) {
    // block_patterns 가 비어있으면 fail-close: std::unexpected 반환
    const std::string tmp_path = "/tmp/test_policy_empty_patterns.yaml";
    {
        std::FILE* f = std::fopen(tmp_path.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs(R"(
global:
  log_level: info

sql_rules:
  block_statements:
    - DROP
  block_patterns: []

procedure_control:
  mode: "whitelist"
  whitelist: []
  block_dynamic_sql: true
  block_create_alter: true

data_protection:
  max_result_rows: 1000
  block_schema_access: true
)", f);
        std::fclose(f);
    }

    const auto result = PolicyLoader::load(tmp_path);
    EXPECT_FALSE(result.has_value())
        << "Empty block_patterns should cause load failure (fail-close)";
    EXPECT_NE(result.error().find("block_patterns"), std::string::npos)
        << "Error message should mention block_patterns";

    std::remove(tmp_path.c_str());
}

TEST(PolicyLoader, LoadFile_TimeoutParsing) {
    // connection_timeout: "30s" → connection_timeout_sec = 30
    const std::string tmp_path = "/tmp/test_policy_timeout.yaml";
    {
        std::FILE* f = std::fopen(tmp_path.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs(R"(
global:
  connection_timeout: 45s

sql_rules:
  block_patterns:
    - "UNION\\s+SELECT"
)", f);
        std::fclose(f);
    }

    const auto result = PolicyLoader::load(tmp_path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().global.connection_timeout_sec, 45u);

    std::remove(tmp_path.c_str());
}

TEST(PolicyLoader, LoadFile_TimeRestriction) {
    // time_restriction 파싱 검증
    const std::string tmp_path = "/tmp/test_policy_time.yaml";
    {
        std::FILE* f = std::fopen(tmp_path.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs(R"(
access_control:
  - user: "readonly"
    source_ip: "192.168.1.0/24"
    allowed_tables: ["users"]
    allowed_operations: ["SELECT"]
    time_restriction:
      allow: "09:00-18:00"
      timezone: "Asia/Seoul"

sql_rules:
  block_patterns:
    - "UNION\\s+SELECT"
)", f);
        std::fclose(f);
    }

    const auto result = PolicyLoader::load(tmp_path);
    ASSERT_TRUE(result.has_value());

    const auto& rules = result.value().access_control;
    ASSERT_EQ(rules.size(), 1u);
    ASSERT_TRUE(rules[0].time_restriction.has_value());
    EXPECT_EQ(rules[0].time_restriction->allow_range, "09:00-18:00");
    EXPECT_EQ(rules[0].time_restriction->timezone, "Asia/Seoul");

    std::remove(tmp_path.c_str());
}

TEST(PolicyLoader, LoadFile_NullTimeRestriction) {
    // time_restriction: null → std::nullopt
    const std::string tmp_path = "/tmp/test_policy_null_time.yaml";
    {
        std::FILE* f = std::fopen(tmp_path.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs(R"(
access_control:
  - user: "admin"
    source_ip: ""
    allowed_tables: ["*"]
    allowed_operations: ["*"]
    time_restriction: null

sql_rules:
  block_patterns:
    - "UNION\\s+SELECT"
)", f);
        std::fclose(f);
    }

    const auto result = PolicyLoader::load(tmp_path);
    ASSERT_TRUE(result.has_value());

    const auto& rules = result.value().access_control;
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_FALSE(rules[0].time_restriction.has_value());

    std::remove(tmp_path.c_str());
}

TEST(PolicyLoader, LoadFile_InvalidYaml_ReturnsError) {
    // 잘못된 YAML 파일
    const std::string tmp_path = "/tmp/test_policy_invalid.yaml";
    {
        std::FILE* f = std::fopen(tmp_path.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("{ invalid yaml: [unclosed", f);
        std::fclose(f);
    }

    const auto result = PolicyLoader::load(tmp_path);
    EXPECT_FALSE(result.has_value());

    std::remove(tmp_path.c_str());
}

// ===========================================================================
// CIDR 매칭 엣지 케이스
// ===========================================================================

TEST(PolicyEngine, CidrMatch_Slash32_ExactIp) {
    auto cfg = make_basic_config();
    cfg->access_control[0].source_ip_cidr = "192.168.1.100/32";  // 정확히 하나의 IP
    PolicyEngine engine(cfg);

    // 정확히 일치하는 IP
    const auto result1 = engine.evaluate(
        make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users"),
        make_session("testuser", "192.168.1.100")
    );
    EXPECT_EQ(result1.action, PolicyAction::kAllow);

    // 다른 IP
    const auto result2 = engine.evaluate(
        make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users"),
        make_session("testuser", "192.168.1.101")
    );
    EXPECT_EQ(result2.action, PolicyAction::kBlock);
    EXPECT_EQ(result2.matched_rule, "no-access-rule");
}

TEST(PolicyEngine, CidrMatch_Slash0_AllIps) {
    auto cfg = make_basic_config();
    cfg->access_control[0].source_ip_cidr = "0.0.0.0/0";  // 모든 IP
    PolicyEngine engine(cfg);

    const auto result = engine.evaluate(
        make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users"),
        make_session("testuser", "1.2.3.4")
    );
    EXPECT_EQ(result.action, PolicyAction::kAllow);
}

TEST(PolicyEngine, CidrMatch_InvalidCidr_Fails) {
    // 잘못된 CIDR → ip_in_cidr() false → 룰 매칭 실패 → kBlock (fail-close)
    auto cfg = make_basic_config();
    cfg->access_control[0].source_ip_cidr = "not-a-cidr";
    PolicyEngine engine(cfg);

    const auto result = engine.evaluate(
        make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users"),
        make_session("testuser", "192.168.1.1")
    );
    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "no-access-rule");
}

// ===========================================================================
// 우회 가능성 문서화 — 알려진 false negative
// ===========================================================================

// [알려진 미탐] 주석 분할: "UN/**/ION SELECT" 는 block_patterns 로 탐지되지 않음
TEST(PolicyEngine, KnownFalseNegative_CommentSplit) {
    PolicyEngine engine(make_basic_config());
    // UN/**/ION SEL/**/ECT 는 UNION\\s+SELECT 패턴에 매칭되지 않음
    const auto query = make_query(
        SqlCommand::kSelect, {"users"},
        "SELECT * FROM users WHERE id=1 UN/**/ION SEL/**/ECT 1,2,3"
    );
    const auto result = engine.evaluate(query, make_session());
    // 이 테스트는 알려진 false negative 를 문서화
    (void)result;
    SUCCEED() << "Comment-split bypass (UN/**/ION SELECT) is a known false negative in block_patterns.";
}

// [알려진 미탐] 인코딩 우회: hex 리터럴/CHAR() 함수 통한 우회
TEST(PolicyEngine, KnownFalseNegative_EncodingBypass) {
    PolicyEngine engine(make_basic_config());
    const auto query = make_query(
        SqlCommand::kSelect, {},
        "SELECT CHAR(85,78,73,79,78,32,83,69,76,69,67,84)"
    );
    const auto result = engine.evaluate(query, make_session());
    (void)result;
    SUCCEED() << "Encoding bypass via CHAR() is a known false negative.";
}

// ===========================================================================
// 3-1. 시간대 경계값: 잘못된 시간 형식 → fail-close
// ===========================================================================

// "25:70-13:00" → 유효하지 않은 시/분 → fail-close
TEST(PolicyEngine, TimeRestriction_InvalidHourMinute_Blocks) {
    auto cfg = make_basic_config();
    TimeRestriction tr{};
    tr.allow_range = "25:70-13:00";
    tr.timezone    = "UTC";
    cfg->access_control[0].time_restriction = tr;
    PolicyEngine engine(cfg);

    const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "time-restriction");
}

// "abc:def-10:00" → 파싱 불가 → fail-close
TEST(PolicyEngine, TimeRestriction_AlphaFormat_Blocks) {
    auto cfg = make_basic_config();
    TimeRestriction tr{};
    tr.allow_range = "abc:def-10:00";
    tr.timezone    = "UTC";
    cfg->access_control[0].time_restriction = tr;
    PolicyEngine engine(cfg);

    const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "time-restriction");
}

// "10:00" (범위 없음 — 단일 시각만) → 파싱 불가 → fail-close
TEST(PolicyEngine, TimeRestriction_NoRangeDash_Blocks) {
    auto cfg = make_basic_config();
    TimeRestriction tr{};
    tr.allow_range = "10:00";  // '-' 없음
    tr.timezone    = "UTC";
    cfg->access_control[0].time_restriction = tr;
    PolicyEngine engine(cfg);

    const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "time-restriction");
}

// 자정을 넘는 시간 범위: "22:00-06:00" 형식이 파싱 성공하는지 확인
// 실제 allow/deny 결과는 실행 시각에 따라 다르므로, 파싱 오류가 아닌
// "Access outside allowed hours" 또는 kAllow 결과여야 한다는 것만 검증
TEST(PolicyEngine, TimeRestriction_MidnightCrossing_Parseable) {
    auto cfg = make_basic_config();
    TimeRestriction tr{};
    tr.allow_range = "22:00-06:00";
    tr.timezone    = "UTC";
    cfg->access_control[0].time_restriction = tr;
    PolicyEngine engine(cfg);

    const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
    const auto result = engine.evaluate(query, make_session());

    // "22:00-06:00" 은 유효한 형식 — 파싱 실패 메시지("Invalid time restriction")가
    // 없어야 한다. 시각에 따라 kAllow 또는 kBlock("Access outside allowed hours") 가능.
    EXPECT_EQ(result.reason.find("Invalid time restriction"), std::string::npos)
        << "22:00-06:00 is valid; should not fail to parse. reason='" << result.reason << "'";
}

// 자정을 넘는 범위에서 "always-allow" 설정: "00:00-23:59" 의 역, "23:59-00:00"
// 1분짜리 범위 → 거의 항상 "Access outside allowed hours" 차단이어야 하나 결정론적이지 않으므로
// 파싱 성공 여부(=parse 오류 아닌 시각 기반 판정)만 검증
TEST(PolicyEngine, TimeRestriction_OneMinuteRange_Parseable) {
    auto cfg = make_basic_config();
    TimeRestriction tr{};
    tr.allow_range = "23:59-00:00";  // 자정을 넘는 1분 범위
    tr.timezone    = "UTC";
    cfg->access_control[0].time_restriction = tr;
    PolicyEngine engine(cfg);

    const auto query  = make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users");
    const auto result = engine.evaluate(query, make_session());

    // 파싱 성공 확인: "Invalid time restriction configuration" 없어야 함
    EXPECT_EQ(result.reason.find("Invalid time restriction"), std::string::npos)
        << "23:59-00:00 is valid; should not fail to parse. reason='" << result.reason << "'";
}

// ===========================================================================
// 3-2. 잘못된 CIDR 입력 형식 (PolicyEngine 관점)
// ===========================================================================

// "999.999.0.0/33" → prefix > 32 → ip_in_cidr() false → no-access-rule (fail-close)
TEST(PolicyEngine, CidrMatch_PrefixOver32_Blocks) {
    auto cfg = make_basic_config();
    cfg->access_control[0].source_ip_cidr = "999.999.0.0/33";
    PolicyEngine engine(cfg);

    const auto result = engine.evaluate(
        make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users"),
        make_session("testuser", "192.168.1.100")
    );
    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "no-access-rule");
}

// "192.168.1.0/abc" → 비숫자 prefix → ip_in_cidr() false → no-access-rule (fail-close)
TEST(PolicyEngine, CidrMatch_NonNumericPrefix_Blocks) {
    auto cfg = make_basic_config();
    cfg->access_control[0].source_ip_cidr = "192.168.1.0/abc";
    PolicyEngine engine(cfg);

    const auto result = engine.evaluate(
        make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users"),
        make_session("testuser", "192.168.1.100")
    );
    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "no-access-rule");
}

// PolicyLoader: 잘못된 CIDR 문자열이 포함된 YAML 파일은 로딩 성공하되
// PolicyEngine에서 해당 규칙을 매칭 실패로 처리하는지 확인 (저장 시 유효성 검사 없음)
TEST(PolicyLoader, LoadFile_InvalidCidr_LoadsSuccessfully) {
    // policy_loader 는 CIDR 유효성 검사를 수행하지 않음 (PolicyEngine 에서 처리)
    // 잘못된 CIDR 이 있어도 로딩 성공 여부를 확인한다.
    const std::string tmp_path = "/tmp/test_policy_invalid_cidr.yaml";
    {
        std::FILE* f = std::fopen(tmp_path.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs(R"(
access_control:
  - user: "admin"
    source_ip: "999.999.0.0/33"
    allowed_tables: ["*"]
    allowed_operations: ["*"]

sql_rules:
  block_patterns:
    - "UNION\\s+SELECT"
)", f);
        std::fclose(f);
    }

    const auto result = PolicyLoader::load(tmp_path);
    // CIDR 유효성 검사는 PolicyEngine 몫; PolicyLoader 는 로딩 성공해야 함
    ASSERT_TRUE(result.has_value())
        << "PolicyLoader should load successfully even with invalid CIDR; error=" << result.error();
    ASSERT_EQ(result.value().access_control.size(), 1u);
    EXPECT_EQ(result.value().access_control[0].source_ip_cidr, "999.999.0.0/33");

    std::remove(tmp_path.c_str());
}

// PolicyLoader: 잘못된 시간 형식 ("25:70-13:00")이 포함된 YAML 로딩
// PolicyLoader 는 시간 파싱을 수행하지 않으므로 로딩 성공 → PolicyEngine 에서 fail-close
TEST(PolicyLoader, LoadFile_InvalidTimeFormat_LoadsSuccessfully) {
    const std::string tmp_path = "/tmp/test_policy_invalid_time.yaml";
    {
        std::FILE* f = std::fopen(tmp_path.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs(R"(
access_control:
  - user: "testuser"
    source_ip: ""
    allowed_tables: ["*"]
    allowed_operations: ["SELECT"]
    time_restriction:
      allow: "25:70-13:00"
      timezone: "UTC"

sql_rules:
  block_patterns:
    - "UNION\\s+SELECT"
)", f);
        std::fclose(f);
    }

    const auto result = PolicyLoader::load(tmp_path);
    // PolicyLoader 는 시간 문자열 파싱을 수행하지 않으므로 로딩 성공
    ASSERT_TRUE(result.has_value())
        << "PolicyLoader should load file with invalid time format; error=" << result.error();
    ASSERT_EQ(result.value().access_control.size(), 1u);
    ASSERT_TRUE(result.value().access_control[0].time_restriction.has_value());
    EXPECT_EQ(result.value().access_control[0].time_restriction->allow_range, "25:70-13:00");

    // PolicyEngine 에서 실제 사용 시 fail-close 검증
    auto cfg = std::make_shared<PolicyConfig>(result.value());
    PolicyEngine engine(cfg);

    // testuser, 빈 CIDR(= 모든 IP 허용)
    const auto session = make_session("testuser", "1.2.3.4");
    const auto query   = make_query(SqlCommand::kSelect, {}, "SELECT 1");
    const auto eval    = engine.evaluate(query, session);

    EXPECT_EQ(eval.action, PolicyAction::kBlock)
        << "Engine should fail-close on invalid time format '25:70-13:00'";
    EXPECT_EQ(eval.matched_rule, "time-restriction");

    std::remove(tmp_path.c_str());
}

// ===========================================================================
// 3-3. blocked_operations vs allowed_operations 우선순위
// 같은 룰에 allowed=[SELECT, UPDATE] + blocked=[UPDATE] → SELECT ALLOW, UPDATE DENY
// ===========================================================================

TEST(PolicyEngine, BlockedOverridesAllowed_UpdateDenied) {
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_operations = {"SELECT", "UPDATE"};
    cfg->access_control[0].blocked_operations = {"UPDATE"};
    PolicyEngine engine(cfg);

    // SELECT 는 allowed 에 있고 blocked 에 없음 → kAllow
    const auto select_result = engine.evaluate(
        make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users"),
        make_session()
    );
    EXPECT_EQ(select_result.action, PolicyAction::kAllow)
        << "SELECT should be allowed (in allowed_ops, not in blocked_ops)";

    // UPDATE 는 allowed 에도 있지만 blocked 에도 있음 → blocked 우선 → kBlock
    const auto update_result = engine.evaluate(
        make_query(SqlCommand::kUpdate, {"users"}, "UPDATE users SET name='a' WHERE id=1"),
        make_session()
    );
    EXPECT_EQ(update_result.action, PolicyAction::kBlock)
        << "UPDATE should be blocked (blocked_ops takes precedence over allowed_ops)";
    EXPECT_EQ(update_result.matched_rule, "blocked-operation");
}

TEST(PolicyEngine, BlockedOverridesAllowed_InsertDenied_WhenBlocked) {
    // INSERT 가 allowed 에 있고 blocked 에도 있으면 → blocked 우선
    auto cfg = make_basic_config();
    cfg->access_control[0].allowed_operations = {"SELECT", "INSERT", "UPDATE"};
    cfg->access_control[0].blocked_operations = {"INSERT"};
    PolicyEngine engine(cfg);

    const auto insert_result = engine.evaluate(
        make_query(SqlCommand::kInsert, {"users"}, "INSERT INTO users VALUES(1,'a')"),
        make_session()
    );
    EXPECT_EQ(insert_result.action, PolicyAction::kBlock);
    EXPECT_EQ(insert_result.matched_rule, "blocked-operation");

    // SELECT 는 정상 허용
    const auto select_result = engine.evaluate(
        make_query(SqlCommand::kSelect, {"users"}, "SELECT * FROM users"),
        make_session()
    );
    EXPECT_EQ(select_result.action, PolicyAction::kAllow);
}

// ===========================================================================
// 3-4. 동적 SQL 차단 — block_dynamic_sql: false 시 PREPARE 허용
// ===========================================================================

TEST(PolicyEngine, DynamicSql_BlockFalse_PrepareAllowed) {
    auto cfg = make_basic_config();
    cfg->procedure_control.block_dynamic_sql = false;
    cfg->access_control[0].allowed_operations = {"SELECT", "PREPARE", "EXECUTE"};
    cfg->access_control[0].allowed_tables     = {"*"};
    PolicyEngine engine(cfg);

    // block_dynamic_sql = false → PREPARE 허용
    const auto query = make_query(SqlCommand::kPrepare, {}, "PREPARE stmt FROM 'SELECT 1'");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kAllow)
        << "PREPARE should be allowed when block_dynamic_sql=false";
}

TEST(PolicyEngine, DynamicSql_BlockFalse_ExecuteAllowed) {
    auto cfg = make_basic_config();
    cfg->procedure_control.block_dynamic_sql = false;
    cfg->access_control[0].allowed_operations = {"SELECT", "PREPARE", "EXECUTE"};
    cfg->access_control[0].allowed_tables     = {"*"};
    PolicyEngine engine(cfg);

    // block_dynamic_sql = false → EXECUTE 허용
    const auto query = make_query(SqlCommand::kExecute, {}, "EXECUTE stmt");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kAllow)
        << "EXECUTE should be allowed when block_dynamic_sql=false";
}

// block_dynamic_sql = true 이고 allowed_operations 에 PREPARE 있어도 차단 (기존 검증 보완)
TEST(PolicyEngine, DynamicSql_BlockTrue_PrepareBlockedEvenIfInAllowed) {
    auto cfg = make_basic_config();
    cfg->procedure_control.block_dynamic_sql = true;
    cfg->access_control[0].allowed_operations = {"SELECT", "PREPARE"};
    cfg->access_control[0].allowed_tables     = {"*"};
    PolicyEngine engine(cfg);

    const auto query = make_query(SqlCommand::kPrepare, {}, "PREPARE stmt FROM 'DROP TABLE x'");
    const auto result = engine.evaluate(query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "procedure-dynamic-sql");
}

// ===========================================================================
// 3-5. config/policy.yaml 실제 로딩 테스트
// ===========================================================================

TEST(PolicyLoader, LoadActualPolicyYaml_Succeeds) {
    // config/policy.yaml 이 있으면 로딩하여 파싱 오류 없음을 확인한다.
    // 파일이 없으면 GTEST_SKIP() 으로 건너뜀.
    const std::string policy_path = "/workspace/config/policy.yaml";

    if (!std::filesystem::exists(policy_path)) {
        GTEST_SKIP() << "config/policy.yaml not found, skipping test";
    }

    const auto result = PolicyLoader::load(policy_path);
    ASSERT_TRUE(result.has_value())
        << "config/policy.yaml should parse successfully. error='" << result.error() << "'";

    const auto& cfg = result.value();
    // 기본 구조 확인
    EXPECT_FALSE(cfg.sql_rules.block_patterns.empty())
        << "config/policy.yaml must have at least one block_pattern";
    EXPECT_FALSE(cfg.access_control.empty())
        << "config/policy.yaml should have at least one access_control rule";
}

// ===========================================================================
// 4. Fail-close 추가 검증
// ===========================================================================

// 빈 정책 (access_control = 0개 룰) → 모든 쿼리 DENY
TEST(PolicyEngine, EmptyAccessControl_BlocksAllQueries) {
    auto cfg = std::make_shared<PolicyConfig>();
    // access_control 비어있음 (0개 룰)
    cfg->sql_rules.block_patterns = {"UNION\\s+SELECT"};  // block_patterns 최소 1개
    PolicyEngine engine(cfg);

    const auto select_query = make_query(SqlCommand::kSelect, {"users"}, "SELECT * FROM users");
    const auto result       = engine.evaluate(select_query, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock)
        << "Empty access_control should block all queries (default deny / fail-close)";
    EXPECT_EQ(result.matched_rule, "no-access-rule");
}

// 빈 정책: 여러 사용자/쿼리 타입 모두 차단
TEST(PolicyEngine, EmptyAccessControl_BlocksMultipleQueryTypes) {
    auto cfg = std::make_shared<PolicyConfig>();
    cfg->sql_rules.block_patterns = {"UNION\\s+SELECT"};
    PolicyEngine engine(cfg);

    const std::vector<std::pair<SqlCommand, std::string>> cmds = {
        {SqlCommand::kSelect,   "SELECT 1"},
        {SqlCommand::kInsert,   "INSERT INTO t VALUES(1)"},
        {SqlCommand::kUpdate,   "UPDATE t SET x=1"},
        {SqlCommand::kDelete,   "DELETE FROM t"},
    };

    for (const auto& [cmd, sql] : cmds) {
        const auto result = engine.evaluate(
            make_query(cmd, {}, sql),
            make_session("anyuser", "1.2.3.4")
        );
        EXPECT_EQ(result.action, PolicyAction::kBlock)
            << "Empty access_control should block " << sql;
        EXPECT_EQ(result.matched_rule, "no-access-rule");
    }
}

// 정책 파일 경로가 존재하지 않을 때 로딩 실패 확인 (기존 테스트 보강)
TEST(PolicyLoader, LoadMissingFile_ReturnsExpectedErrorString) {
    const auto result = PolicyLoader::load("/nonexistent/absolute/path/to/policy.yaml");
    ASSERT_FALSE(result.has_value());
    // 에러 메시지가 비어있지 않고 경로 정보를 포함해야 한다
    EXPECT_FALSE(result.error().empty());
    EXPECT_NE(result.error().find("policy_loader"), std::string::npos)
        << "Error message should contain 'policy_loader' prefix";
}

// evaluate_error: kUnsupportedCommand 에러 코드
TEST(PolicyEngine, EvaluateError_UnsupportedCommand_Blocks) {
    PolicyEngine engine(make_basic_config());
    ParseError err{};
    err.code    = ParseErrorCode::kUnsupportedCommand;
    err.message = "unsupported MySQL command type";

    const auto result = engine.evaluate_error(err, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "parse-error");
    EXPECT_NE(result.reason.find("unsupported MySQL command type"), std::string::npos);
}

// evaluate_error: 빈 에러 메시지도 안전하게 처리
TEST(PolicyEngine, EvaluateError_EmptyMessage_StillBlocks) {
    PolicyEngine engine(make_basic_config());
    ParseError err{};
    err.code    = ParseErrorCode::kInvalidSql;
    err.message = "";  // 빈 메시지

    const auto result = engine.evaluate_error(err, make_session());

    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "parse-error");
}

// 전체 규칙이 있지만 wildcard user 가 아닌 특정 사용자만 있고
// 다른 사용자가 요청하면 차단 — 기존 확인이지만 session_id 다양화
TEST(PolicyEngine, NoMatchingRule_UnknownUser_BlocksWithDifferentSessions) {
    PolicyEngine engine(make_basic_config());

    for (std::uint64_t sid = 100; sid < 105; ++sid) {
        const auto session = make_session("attacker", "10.10.10.10", sid);
        const auto query   = make_query(SqlCommand::kSelect, {"users"}, "SELECT * FROM users");
        const auto result  = engine.evaluate(query, session);

        EXPECT_EQ(result.action, PolicyAction::kBlock)
            << "Unrecognized user should be blocked, session=" << sid;
        EXPECT_EQ(result.matched_rule, "no-access-rule");
    }
}

// ===========================================================================
// 추가 CIDR 경계값 테스트
// ===========================================================================

// 음수 prefix: "192.168.1.0/-1" → 잘못된 형식 → fail-close
TEST(PolicyEngine, CidrMatch_NegativePrefix_Blocks) {
    auto cfg = make_basic_config();
    cfg->access_control[0].source_ip_cidr = "192.168.1.0/-1";
    PolicyEngine engine(cfg);

    const auto result = engine.evaluate(
        make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users"),
        make_session("testuser", "192.168.1.1")
    );
    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "no-access-rule");
}

// CIDR 슬래시만 있고 prefix 없음: "192.168.1.0/" → 잘못된 형식 → fail-close
TEST(PolicyEngine, CidrMatch_EmptyPrefix_Blocks) {
    auto cfg = make_basic_config();
    cfg->access_control[0].source_ip_cidr = "192.168.1.0/";
    PolicyEngine engine(cfg);

    const auto result = engine.evaluate(
        make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users"),
        make_session("testuser", "192.168.1.1")
    );
    EXPECT_EQ(result.action, PolicyAction::kBlock);
    EXPECT_EQ(result.matched_rule, "no-access-rule");
}

// CIDR 경계: /16 범위 (255.255.0.0 마스크)
TEST(PolicyEngine, CidrMatch_Slash16_RangeCheck) {
    auto cfg = make_basic_config();
    cfg->access_control[0].source_ip_cidr = "10.10.0.0/16";
    PolicyEngine engine(cfg);

    // 범위 안: 10.10.100.200
    const auto in_result = engine.evaluate(
        make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users"),
        make_session("testuser", "10.10.100.200")
    );
    EXPECT_EQ(in_result.action, PolicyAction::kAllow);

    // 범위 밖: 10.11.0.1
    const auto out_result = engine.evaluate(
        make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users"),
        make_session("testuser", "10.11.0.1")
    );
    EXPECT_EQ(out_result.action, PolicyAction::kBlock);
    EXPECT_EQ(out_result.matched_rule, "no-access-rule");
}

// ===========================================================================
// Hot Reload 추가 검증: 새 config 로 reload 후 여러 evaluate 호출
// ===========================================================================

TEST(PolicyEngine, Reload_MultipleCalls_LastConfigApplied) {
    PolicyEngine engine(make_basic_config());

    // 1차 reload: admin 허용 config
    auto cfg1 = std::make_shared<PolicyConfig>();
    cfg1->sql_rules.block_patterns = {"UNION\\s+SELECT"};
    AccessRule admin_rule{};
    admin_rule.user             = "admin";
    admin_rule.source_ip_cidr   = "";
    admin_rule.allowed_tables   = {"*"};
    admin_rule.allowed_operations = {"SELECT"};
    cfg1->access_control.push_back(admin_rule);
    engine.reload(cfg1);

    // admin SELECT → allow
    {
        const auto result = engine.evaluate(
            make_query(SqlCommand::kSelect, {"users"}, "SELECT 1"),
            make_session("admin", "1.2.3.4")
        );
        EXPECT_EQ(result.action, PolicyAction::kAllow);
    }

    // 2차 reload: nullptr (fail-close)
    engine.reload(nullptr);

    // admin SELECT → block (no-config)
    {
        const auto result = engine.evaluate(
            make_query(SqlCommand::kSelect, {"users"}, "SELECT 1"),
            make_session("admin", "1.2.3.4")
        );
        EXPECT_EQ(result.action, PolicyAction::kBlock);
        EXPECT_EQ(result.matched_rule, "no-config");
    }

    // 3차 reload: 원래 testuser 허용 config
    engine.reload(make_basic_config());

    // testuser SELECT → allow
    {
        const auto result = engine.evaluate(
            make_query(SqlCommand::kSelect, {"users"}, "SELECT id FROM users"),
            make_session()
        );
        EXPECT_EQ(result.action, PolicyAction::kAllow);
    }
}
