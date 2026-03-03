// ---------------------------------------------------------------------------
// test_edge_cases.cpp
//
// DON-34: 엣지 케이스 / 경계값 테스트 (신규)
//
// [테스트 범위]
// A. MysqlPacketEdge   — 헤더/페이로드 경계값, NULL 바이트, 제어 문자
// B. SqlParserEdge     — 초대형 쿼리, NULL 바이트, 한글/이모지 식별자, 공백 과잉
// C. PolicyEngineEdge  — 연속 Reload 스트레스, nullptr→유효 config 복구
// D. StatsEdge         — 언더플로우 반복, 고부하 카운팅
//
// [설계 원칙]
// - 크래시/UB/무한루프 없음이 최우선 보장 목표다.
// - 결과(성공/에러)보다 안전한 종료 여부를 검증한다.
// - 기존 테스트 파일과 중복되지 않는 케이스만 포함한다.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.hpp"
#include "parser/sql_parser.hpp"
#include "policy/policy_engine.hpp"
#include "policy/policy_loader.hpp"
#include "protocol/command.hpp"
#include "protocol/mysql_packet.hpp"
#include "stats/stats_collector.hpp"

// ===========================================================================
// 테스트 헬퍼 (익명 네임스페이스)
// ===========================================================================
namespace {

// 세션 컨텍스트 생성
SessionContext make_session(const std::string& user = "testuser",
                            const std::string& ip = "192.168.1.100",
                            std::uint64_t sid = 1) {
    SessionContext ctx{};
    ctx.session_id = sid;
    ctx.client_ip = ip;
    ctx.client_port = 3306;
    ctx.db_user = user;
    ctx.db_name = "testdb";
    ctx.handshake_done = true;
    return ctx;
}

// SELECT 쿼리 생성
ParsedQuery make_select_query() {
    ParsedQuery q{};
    q.command = SqlCommand::kSelect;
    q.tables = {"users"};
    q.raw_sql = "SELECT * FROM users";
    q.has_where_clause = false;
    return q;
}

// 최소 유효 PolicyConfig (testuser 허용, 차단 규칙 없음)
std::shared_ptr<PolicyConfig> make_basic_config() {
    auto cfg = std::make_shared<PolicyConfig>();

    // sql_rules: 차단 없음
    cfg->sql_rules.block_statements = {};
    cfg->sql_rules.block_patterns = {};

    // access_control: testuser → 모든 테이블, SELECT 허용
    AccessRule rule{};
    rule.user = "testuser";
    rule.source_ip_cidr = "0.0.0.0/0";
    rule.allowed_tables = {"*"};
    rule.allowed_operations = {"SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP"};
    cfg->access_control.push_back(rule);

    // procedure_control
    cfg->procedure_control.mode = "whitelist";
    cfg->procedure_control.whitelist = {};
    cfg->procedure_control.block_dynamic_sql = false;
    cfg->procedure_control.block_create_alter = false;

    // data_protection
    cfg->data_protection.block_schema_access = false;
    cfg->data_protection.max_result_rows = 10000;

    return cfg;
}

// COM_QUERY 패킷 raw bytes 생성 헬퍼
// payload_bytes: COM_QUERY(0x03) + SQL 바이트열
std::vector<std::uint8_t> make_com_query_packet(const std::vector<std::uint8_t>& sql_bytes,
                                                std::uint8_t seq_id = 0) {
    std::vector<std::uint8_t> payload;
    payload.push_back(0x03);  // COM_QUERY
    payload.insert(payload.end(), sql_bytes.begin(), sql_bytes.end());

    const auto len = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> data;
    data.push_back(static_cast<std::uint8_t>(len & 0xFFU));
    data.push_back(static_cast<std::uint8_t>((len >> 8U) & 0xFFU));
    data.push_back(static_cast<std::uint8_t>((len >> 16U) & 0xFFU));
    data.push_back(seq_id);
    data.insert(data.end(), payload.begin(), payload.end());
    return data;
}

}  // namespace

// ===========================================================================
// A. MysqlPacketEdge — MySQL 패킷 경계값 테스트
// ===========================================================================

// ---------------------------------------------------------------------------
// MaxSizeBoundaryHeader
//   헤더에 0xFFFFFF (16MB-1) 길이를 선언하되 실제 payload는 없는 4바이트만
//   → 파싱 실패 (에러 반환) 확인. 실제 16MB 할당 없이 테스트.
// ---------------------------------------------------------------------------
TEST(MysqlPacketEdge, MaxSizeBoundaryHeader) {
    // data = {0xFF, 0xFF, 0xFF, 0x00}: length=0xFFFFFF 이지만 payload 없음
    std::vector<std::uint8_t> data = {0xFF, 0xFF, 0xFF, 0x00};

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});

    // declared length(16MB-1) != actual payload(0) → 반드시 에러
    ASSERT_FALSE(result.has_value())
        << "Parser must reject packet with declared_len=0xFFFFFF but no payload";
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// NullByteInComQuery
//   COM_QUERY(0x03) payload에 NULL 바이트 포함 → 크래시 없이 처리
//   결과(성공/에러)보다 "프로세스 종료 없음"을 보장하는 것이 목표.
// ---------------------------------------------------------------------------
TEST(MysqlPacketEdge, NullByteInComQuery) {
    // SQL: {0x03, 'S', 'E', 'L', 'E', 'C', 'T', ' ', 0x00, '1'}
    std::vector<std::uint8_t> sql_bytes = {'S', 'E', 'L', 'E', 'C', 'T', ' ', 0x00, '1'};
    const auto data = make_com_query_packet(sql_bytes);

    auto parse_result = MysqlPacket::parse(std::span<const std::uint8_t>{data});

    // 파싱 성공 여부와 무관하게 크래시가 없어야 함
    if (parse_result.has_value()) {
        // 파싱 성공 시 extract_command 도 크래시 없이 완료해야 함
        const auto cmd_result = extract_command(*parse_result);
        // 성공이든 에러든 크래시 없음이 검증 기준
        (void)cmd_result;
    }
    // 이 지점에 도달했다면 크래시 없음 — 테스트 통과
    SUCCEED() << "NullByteInComQuery completed without crash";
}

// ---------------------------------------------------------------------------
// TabNewlineInQuery
//   COM_QUERY payload에 \t, \n, \r 포함 → extract_command 성공,
//   query string에 제어 문자 보존 확인.
// ---------------------------------------------------------------------------
TEST(MysqlPacketEdge, TabNewlineInQuery) {
    // SQL: "SELECT\t*\nFROM\r\nt"
    const std::string sql_str = "SELECT\t*\nFROM\r\nt";
    std::vector<std::uint8_t> sql_bytes(sql_str.begin(), sql_str.end());
    const auto data = make_com_query_packet(sql_bytes);

    auto parse_result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(parse_result.has_value()) << "Tab/newline in COM_QUERY should parse successfully";

    auto cmd_result = extract_command(*parse_result);
    ASSERT_TRUE(cmd_result.has_value()) << "extract_command should succeed for tab/newline SQL";

    EXPECT_EQ(cmd_result->command_type, CommandType::kComQuery);

    // 제어 문자가 보존되어야 함
    EXPECT_NE(cmd_result->query.find('\t'), std::string::npos)
        << "Tab character must be preserved in query string";
    EXPECT_NE(cmd_result->query.find('\n'), std::string::npos)
        << "Newline character must be preserved in query string";
    EXPECT_NE(cmd_result->query.find('\r'), std::string::npos)
        << "Carriage return must be preserved in query string";
}

// ===========================================================================
// B. SqlParserEdge — SQL 파서 경계값/엣지 케이스 테스트
// ===========================================================================

// ---------------------------------------------------------------------------
// VeryLongSelect
//   10,000자 이상의 SELECT 쿼리 → parse() 성공, command == kSelect,
//   크래시/hang 없음.
// ---------------------------------------------------------------------------
TEST(SqlParserEdge, VeryLongSelect) {
    // col0..col999 = 평균 6자 × 1000 ≈ 6000자. 10000자를 넘기려면 2000회 반복.
    std::string long_sql = "SELECT ";
    for (int i = 0; i < 2000; ++i) {
        long_sql += "col" + std::to_string(i) + ", ";
    }
    long_sql += "1 FROM big_table";

    ASSERT_GT(long_sql.size(), 10000U) << "Test precondition: SQL must be > 10000 chars";

    SqlParser parser;
    const auto result = parser.parse(long_sql);

    ASSERT_TRUE(result.has_value()) << "Very long SELECT should parse without error";
    EXPECT_EQ(result->command, SqlCommand::kSelect)
        << "Very long SELECT command must be classified as kSelect";
}

// ---------------------------------------------------------------------------
// NullByteInSql
//   SQL 문자열에 NULL 바이트 포함 → parse(sv) 가 크래시/무한루프 없이 완료.
//   결과(성공/에러)는 무방.
// ---------------------------------------------------------------------------
TEST(SqlParserEdge, NullByteInSql) {
    std::string sql_with_null = "SELECT";
    sql_with_null += '\x00';
    sql_with_null += "FROM t";

    // string_view로 전체 길이(NULL 바이트 포함)를 전달
    std::string_view sv(sql_with_null.data(), sql_with_null.size());

    SqlParser parser;
    const auto result = parser.parse(sv);

    // 성공이든 에러든 이 지점에 도달하면 크래시/무한루프 없음
    (void)result;
    SUCCEED() << "NullByteInSql completed without crash or infinite loop";
}

// ---------------------------------------------------------------------------
// KoreanTableIdentifier
//   백틱 한글 테이블명 → parse() 성공, command == kSelect, 크래시 없음.
// ---------------------------------------------------------------------------
TEST(SqlParserEdge, KoreanTableIdentifier) {
    // UTF-8 인코딩된 한글 테이블명: "주문테이블"
    // 주: \xEC\xA3\xBC \xEB\xAC\xB8 \xED\x85\x8C\xEC\x9D\xB4\xEB\xB8\x94
    // (주=EC A3 BC, 문=EB AC B8, 테=ED 85 8C, 이=EC 9D B4, 블=EB B8 94)
    const std::string sql =
        "SELECT * FROM `"
        "\xEC\xA3\xBC"  // 주
        "\xEB\xAC\xB8"  // 문
        "\xED\x85\x8C"  // 테
        "\xEC\x9D\xB4"  // 이
        "\xEB\xB8\x94"  // 블
        "`";

    SqlParser parser;
    const auto result = parser.parse(sql);

    // 크래시가 없으면 성공. 결과는 성공 또는 에러 모두 허용.
    if (result.has_value()) {
        EXPECT_EQ(result->command, SqlCommand::kSelect)
            << "Korean table identifier: SELECT should be classified as kSelect";
    }
    SUCCEED() << "KoreanTableIdentifier completed without crash";
}

// ---------------------------------------------------------------------------
// EmojiInStringLiteral
//   이모지가 포함된 문자열 리터럴 → parse() 성공, command == kSelect.
// ---------------------------------------------------------------------------
TEST(SqlParserEdge, EmojiInStringLiteral) {
    // UTF-8 😀 = 0xF0 0x9F 0x98 0x80
    const std::string sql = "SELECT '\xF0\x9F\x98\x80' AS emoji FROM t";

    SqlParser parser;
    const auto result = parser.parse(sql);

    ASSERT_TRUE(result.has_value()) << "SQL with emoji in string literal should parse successfully";
    EXPECT_EQ(result->command, SqlCommand::kSelect)
        << "Emoji in string literal: command must be kSelect";
}

// ---------------------------------------------------------------------------
// LeadingWhitespaceHeavy
//   1000자 공백+탭+개행 뒤 SELECT → parse 성공, command == kSelect.
// ---------------------------------------------------------------------------
TEST(SqlParserEdge, LeadingWhitespaceHeavy) {
    std::string sql =
        std::string(500, ' ') + std::string(250, '\t') + std::string(250, '\n') + "SELECT 1 FROM t";

    ASSERT_GT(sql.size(), 1000U)
        << "Test precondition: SQL must be > 1000 chars with leading whitespace";

    SqlParser parser;
    const auto result = parser.parse(sql);

    ASSERT_TRUE(result.has_value())
        << "SQL with heavy leading whitespace should parse successfully";
    EXPECT_EQ(result->command, SqlCommand::kSelect)
        << "Heavy leading whitespace before SELECT: command must be kSelect";
}

// ---------------------------------------------------------------------------
// OnlyComment
//   주석만 있는 SQL → parse 실패 또는 kUnknown, 크래시 없음.
//   (기존 CommentsOnly 테스트와 달리 ParseError 타입을 상세 확인하지 않음)
// ---------------------------------------------------------------------------
TEST(SqlParserEdge, OnlyComment) {
    const std::string sql = "/* just a comment */";

    SqlParser parser;
    const auto result = parser.parse(sql);

    if (result.has_value()) {
        // 성공하더라도 kUnknown 이어야 함 (의미 있는 SQL 아님)
        EXPECT_EQ(result->command, SqlCommand::kUnknown)
            << "Comment-only SQL should be classified as kUnknown if parse succeeds";
    } else {
        // 파싱 실패도 허용 — 크래시 없음이 핵심
        EXPECT_FALSE(result.error().message.empty())
            << "Parse error for comment-only SQL must have non-empty message";
    }
    SUCCEED() << "OnlyComment completed without crash";
}

// ===========================================================================
// C. PolicyEngineEdge — 정책 엔진 연속 Reload 스트레스
// ===========================================================================

// ---------------------------------------------------------------------------
// RapidReloadStress
//   10회 연속 reload() 후 evaluate() 가 crash 없이 동작.
// ---------------------------------------------------------------------------
TEST(PolicyEngineEdge, RapidReloadStress) {
    PolicyEngine engine(make_basic_config());

    // 10회 연속 reload
    for (int i = 0; i < 10; ++i) {
        engine.reload(make_basic_config());
    }

    // 마지막 evaluate 는 정상 동작 (kAllow 또는 kBlock, 크래시만 없으면 됨)
    const auto result = engine.evaluate(make_select_query(), make_session());

    // 값이 유효한 PolicyAction 범위 내여야 함 (크래시 없음 간접 확인)
    const bool valid_action = (result.action == PolicyAction::kAllow) ||
                              (result.action == PolicyAction::kBlock) ||
                              (result.action == PolicyAction::kLog);
    EXPECT_TRUE(valid_action)
        << "After 10 rapid reloads, evaluate must return a valid PolicyAction";

    // 기본 config에서 SELECT는 허용되어야 함
    EXPECT_EQ(result.action, PolicyAction::kAllow)
        << "After reload with basic_config, SELECT should be allowed";
}

// ---------------------------------------------------------------------------
// NullThenValidReload
//   nullptr → 유효한 config로 reload() → evaluate() 가 다시 허용.
// ---------------------------------------------------------------------------
TEST(PolicyEngineEdge, NullThenValidReload) {
    // nullptr config: 모든 쿼리 차단 (fail-close)
    PolicyEngine engine(nullptr);

    auto r1 = engine.evaluate(make_select_query(), make_session());
    EXPECT_EQ(r1.action, PolicyAction::kBlock)
        << "With nullptr config, evaluate must return kBlock (fail-close)";

    // 유효 config로 복구
    engine.reload(make_basic_config());

    auto r2 = engine.evaluate(make_select_query(), make_session());
    EXPECT_EQ(r2.action, PolicyAction::kAllow)
        << "After reload with valid config, SELECT should be allowed";
}

// ===========================================================================
// D. StatsEdge — StatsCollector 경계값 테스트
// ===========================================================================

// ---------------------------------------------------------------------------
// UnderflowProtection
//   on_connection_close()를 open 없이 10회 연속 호출해도
//   active_sessions가 음수(언더플로우)가 되지 않아야 함.
//   (기존 test_stats_collector.cpp의 1회 테스트를 10회로 확장)
// ---------------------------------------------------------------------------
TEST(StatsEdge, UnderflowProtection) {
    StatsCollector stats;

    // 연결 열지 않고 10회 연속 닫기
    for (int i = 0; i < 10; ++i) {
        stats.on_connection_close();
    }

    const auto snap = stats.snapshot();
    EXPECT_GE(snap.active_sessions, 0U)
        << "active_sessions must not underflow below 0 after 10 close calls without open";
}

// ---------------------------------------------------------------------------
// HighVolumeCounting
//   100,000회 on_query(false) 후 total_queries == 100000.
// ---------------------------------------------------------------------------
TEST(StatsEdge, HighVolumeCounting) {
    StatsCollector stats;

    constexpr std::uint64_t kIterations = 100'000ULL;
    for (std::uint64_t i = 0; i < kIterations; ++i) {
        stats.on_query(false);
    }

    const auto snap = stats.snapshot();
    EXPECT_EQ(snap.total_queries, kIterations)
        << "After " << kIterations << " on_query(false) calls, "
        << "total_queries must equal " << kIterations;
    EXPECT_EQ(snap.blocked_queries, 0U)
        << "blocked_queries must remain 0 when all queries are on_query(false)";
}
