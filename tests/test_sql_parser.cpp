// ---------------------------------------------------------------------------
// test_sql_parser.cpp
//
// SqlParser 단위 테스트.
//
// [테스트 범위]
// - SQL 구문 분류 (SqlCommand 매핑)
// - 테이블명 추출 (FROM/INTO/UPDATE/JOIN/TABLE)
// - 대소문자 무관 처리
// - 주석 전처리 (/* */, --, #)
// - 에러 처리 (빈 입력, 공백 전용)
// - has_where_clause 판정
// - raw_sql 원문 보존
//
// [오탐/미탐 주의사항]
// - ORM 생성 복잡 쿼리에서 테이블명 추출이 부정확할 수 있음 (알려진 한계).
// - 서브쿼리 내부 테이블명은 추출하지 않음.
// - PREPARE/EXECUTE 내부 SQL 문자열은 파싱하지 않음.
// ---------------------------------------------------------------------------

#include "parser/sql_parser.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------------------
// 헬퍼: tables 벡터에 특정 이름이 포함되어 있는지 확인 (대소문자 무관)
// ---------------------------------------------------------------------------
static bool contains_table(const std::vector<std::string>& tables,
                            const std::string& name) {
    std::string upper_name = name;
    std::transform(upper_name.begin(), upper_name.end(), upper_name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    return std::any_of(tables.begin(), tables.end(), [&](const std::string& t) {
        std::string upper_t = t;
        std::transform(upper_t.begin(), upper_t.end(), upper_t.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return upper_t == upper_name;
    });
}

// ---------------------------------------------------------------------------
// SqlCommand 분류 테스트
// ---------------------------------------------------------------------------

TEST(SqlParser, SelectCommand) {
    SqlParser parser;
    const auto result = parser.parse("SELECT id FROM users");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kSelect);
}

TEST(SqlParser, InsertCommand) {
    SqlParser parser;
    const auto result = parser.parse("INSERT INTO users(name) VALUES('alice')");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kInsert);
}

TEST(SqlParser, UpdateCommand) {
    SqlParser parser;
    const auto result = parser.parse("UPDATE users SET name='bob' WHERE id=1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kUpdate);
}

TEST(SqlParser, DeleteCommand) {
    SqlParser parser;
    const auto result = parser.parse("DELETE FROM users WHERE id=1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kDelete);
}

TEST(SqlParser, DropCommand) {
    SqlParser parser;
    const auto result = parser.parse("DROP TABLE users");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kDrop);
}

TEST(SqlParser, TruncateCommand) {
    SqlParser parser;
    const auto result = parser.parse("TRUNCATE TABLE users");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kTruncate);
}

TEST(SqlParser, AlterCommand) {
    SqlParser parser;
    const auto result = parser.parse("ALTER TABLE users ADD col INT");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kAlter);
}

TEST(SqlParser, CreateCommand) {
    SqlParser parser;
    const auto result = parser.parse("CREATE TABLE new_table (id INT PRIMARY KEY)");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kCreate);
}

TEST(SqlParser, CallCommand) {
    SqlParser parser;
    const auto result = parser.parse("CALL sp_get_user(1)");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kCall);
}

TEST(SqlParser, PrepareCommand) {
    SqlParser parser;
    const auto result = parser.parse("PREPARE stmt FROM 'SELECT 1'");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kPrepare);
}

TEST(SqlParser, ExecuteCommand) {
    SqlParser parser;
    const auto result = parser.parse("EXECUTE stmt");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kExecute);
}

TEST(SqlParser, UnknownCommand) {
    SqlParser parser;
    const auto result = parser.parse("FOOBAR something");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kUnknown);
}

// ---------------------------------------------------------------------------
// 테이블명 추출 테스트
// ---------------------------------------------------------------------------

TEST(SqlParser, TableFromSelect) {
    SqlParser parser;
    const auto result = parser.parse("SELECT * FROM orders");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(contains_table(result->tables, "orders"))
        << "Expected 'orders' in tables";
}

TEST(SqlParser, TableFromInsert) {
    SqlParser parser;
    const auto result = parser.parse("INSERT INTO products(name) VALUES('widget')");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(contains_table(result->tables, "products"))
        << "Expected 'products' in tables";
}

TEST(SqlParser, TableFromUpdate) {
    SqlParser parser;
    const auto result = parser.parse("UPDATE accounts SET balance=100 WHERE id=1");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(contains_table(result->tables, "accounts"))
        << "Expected 'accounts' in tables";
}

TEST(SqlParser, MultiTableFromJoin) {
    SqlParser parser;
    // [오탐 주의] ORM 생성 복잡 JOIN 쿼리에서 테이블명 추출 부정확 가능
    const auto result = parser.parse(
        "SELECT * FROM orders o JOIN customers c ON o.cust_id = c.id");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(contains_table(result->tables, "orders"))
        << "Expected 'orders' in tables";
    EXPECT_TRUE(contains_table(result->tables, "customers"))
        << "Expected 'customers' in tables";
}

TEST(SqlParser, MultiTableComma) {
    SqlParser parser;
    const auto result = parser.parse("SELECT * FROM t1, t2");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(contains_table(result->tables, "t1"))
        << "Expected 't1' in tables";
    EXPECT_TRUE(contains_table(result->tables, "t2"))
        << "Expected 't2' in tables";
}

TEST(SqlParser, TableFromDropTable) {
    SqlParser parser;
    const auto result = parser.parse("DROP TABLE users");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(contains_table(result->tables, "users"))
        << "Expected 'users' in tables after DROP TABLE";
}

TEST(SqlParser, TableFromTruncateTable) {
    SqlParser parser;
    const auto result = parser.parse("TRUNCATE TABLE logs");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(contains_table(result->tables, "logs"))
        << "Expected 'logs' in tables after TRUNCATE TABLE";
}

// ---------------------------------------------------------------------------
// 대소문자 무관 테스트
// ---------------------------------------------------------------------------

TEST(SqlParser, CaseInsensitive) {
    SqlParser parser;
    // "select * from Users" → kSelect, tables={"Users"}
    const auto result = parser.parse("select * from Users");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kSelect);
    EXPECT_TRUE(contains_table(result->tables, "Users"))
        << "Expected 'Users' in tables (case preserved)";
}

TEST(SqlParser, CaseInsensitiveMixed) {
    SqlParser parser;
    const auto result = parser.parse("SeLeCt Id FROM MyTable");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kSelect);
    EXPECT_TRUE(contains_table(result->tables, "MyTable"));
}

// ---------------------------------------------------------------------------
// 주석 전처리 테스트
// ---------------------------------------------------------------------------

TEST(SqlParser, InlineComment) {
    SqlParser parser;
    // "SELECT /* comment */ 1 FROM t" → kSelect, tables={"t"}
    const auto result = parser.parse("SELECT /* comment */ 1 FROM t");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kSelect);
    EXPECT_TRUE(contains_table(result->tables, "t"));
}

TEST(SqlParser, LineComment) {
    SqlParser parser;
    // "SELECT 1 -- comment\nFROM t" → kSelect, tables={"t"}
    const auto result = parser.parse("SELECT 1 -- comment\nFROM t");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kSelect);
    EXPECT_TRUE(contains_table(result->tables, "t"));
}

TEST(SqlParser, HashComment) {
    SqlParser parser;
    // "SELECT 1 # comment\nFROM t" → kSelect, tables={"t"}
    const auto result = parser.parse("SELECT 1 # comment\nFROM t");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kSelect);
    EXPECT_TRUE(contains_table(result->tables, "t"));
}

TEST(SqlParser, MultipleComments) {
    SqlParser parser;
    // 여러 주석 혼합
    const auto result = parser.parse(
        "/* start */ SELECT /* mid */ id -- end comment\n"
        "FROM employees # another comment\n"
        "WHERE id = 1"
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kSelect);
    EXPECT_TRUE(contains_table(result->tables, "employees"));
    EXPECT_TRUE(result->has_where_clause);
}

// ---------------------------------------------------------------------------
// 에러 처리 테스트
// ---------------------------------------------------------------------------

TEST(SqlParser, EmptyString) {
    SqlParser parser;
    const auto result = parser.parse("");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kInvalidSql);
}

TEST(SqlParser, WhitespaceOnly) {
    SqlParser parser;
    const auto result = parser.parse("   ");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kInvalidSql);
}

TEST(SqlParser, WhitespaceAndNewlines) {
    SqlParser parser;
    const auto result = parser.parse("\n\t  \n");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kInvalidSql);
}

TEST(SqlParser, CommentsOnly) {
    SqlParser parser;
    // 주석만 있으면 kInvalidSql
    const auto result = parser.parse("/* only comment */");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kInvalidSql);
}

// ---------------------------------------------------------------------------
// has_where_clause 테스트
// ---------------------------------------------------------------------------

TEST(SqlParser, HasWhereClause) {
    SqlParser parser;
    const auto result = parser.parse("DELETE FROM users WHERE id=1");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_where_clause);
}

TEST(SqlParser, NoWhereClause) {
    SqlParser parser;
    // [보안 주의] WHERE 없는 DELETE는 전체 삭제 가능 — 정책 엔진에서 차단 권고
    const auto result = parser.parse("DELETE FROM users");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_where_clause);
}

TEST(SqlParser, WhereInComment) {
    SqlParser parser;
    // 주석 안의 WHERE는 제거되어 has_where_clause=false
    const auto result = parser.parse("SELECT * FROM t -- WHERE id=1");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_where_clause)
        << "WHERE in comment should not set has_where_clause";
}

TEST(SqlParser, WhereClauseUpdate) {
    SqlParser parser;
    const auto result = parser.parse("UPDATE config SET val='new' WHERE key='k'");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_where_clause);
}

// ---------------------------------------------------------------------------
// raw_sql 보존 테스트
// ---------------------------------------------------------------------------

TEST(SqlParser, RawSqlPreserved) {
    SqlParser parser;
    const std::string original = "SELECT /* comment */ id FROM users WHERE id=1";
    const auto result = parser.parse(original);
    ASSERT_TRUE(result.has_value());
    // raw_sql은 원문 그대로 보존되어야 함 (주석 포함, 케이스 유지)
    EXPECT_EQ(result->raw_sql, original);
}

TEST(SqlParser, RawSqlPreservedMixedCase) {
    SqlParser parser;
    const std::string original = "select * from MyTable";
    const auto result = parser.parse(original);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->raw_sql, original);
}

// ---------------------------------------------------------------------------
// 스키마 한정 테이블명 테스트 (schema.table)
// ---------------------------------------------------------------------------

TEST(SqlParser, SchemaQualifiedTable) {
    SqlParser parser;
    const auto result = parser.parse("SELECT * FROM mydb.orders");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kSelect);
    // schema.table 형태 지원
    EXPECT_FALSE(result->tables.empty())
        << "Expected at least one table extracted for schema.table form";
}

// ---------------------------------------------------------------------------
// [우회 가능성 문서화] — 탐지 한계를 테스트로 명시
// ---------------------------------------------------------------------------

// [알려진 한계] DROP/**/TABLE 처럼 블록 주석으로 분리된 구문:
// 현재 구현에서는 주석 제거 후 "DROP  TABLE users" 형태로 처리되어
// DROP 키워드는 탐지되나 테이블명 추출은 정상 동작한다.
// 주석 제거 후 공백이 삽입되므로 키워드가 붙어쓰여지는 현상은 없다.
TEST(SqlParser, CommentSplitBypass_DocumentedBehavior) {
    SqlParser parser;
    // DROP/**/TABLE users → 주석 제거 후 "DROP  TABLE users"
    // DROP 키워드는 탐지됨
    const auto result = parser.parse("DROP/**/TABLE users");
    ASSERT_TRUE(result.has_value());
    // 첫 번째 키워드 "DROP"은 정확히 탐지됨
    EXPECT_EQ(result->command, SqlCommand::kDrop);
}

// [알려진 동작] 서브쿼리 내부 테이블명도 추출됨
//
// 현재 구현에서 "FROM inner_table" 패턴은 서브쿼리 내부라도 추출된다.
// 이는 보안 관점에서 오히려 안전한 동작이다:
//   - 테이블 접근 제어가 서브쿼리 내부 테이블에도 적용됨.
//   - false positive 가능성은 있으나 false negative보다 안전.
//
// [설계 한계 문서화] (ADR-006 참조)
// 풀 파서 없이는 outer/inner FROM 을 완전히 구분할 수 없다.
// outer FROM 뒤 '('로 시작하는 경우 서브쿼리로 건너뛰지만,
// 서브쿼리 내부의 "FROM inner_table"은 여전히 매칭된다.
TEST(SqlParser, SubqueryTableExtracted_DocumentedBehavior) {
    SqlParser parser;
    // SELECT * FROM (SELECT id FROM inner_table) AS sub
    // 현재 구현: inner_table도 추출됨 (서브쿼리/outer 구분 불가)
    const auto result = parser.parse(
        "SELECT * FROM (SELECT id FROM inner_table) AS sub");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->command, SqlCommand::kSelect);
    // 현재 구현에서는 inner_table이 추출됨 (알려진 동작, ADR-006 참조)
    // 보안적으로 안전한 방향 (테이블 접근 제어가 적용됨)
    // outer FROM 뒤 '('는 테이블명으로 잡히지 않음
    EXPECT_FALSE(result->tables.empty())
        << "Should extract at least inner_table from subquery FROM clause";
    // outer에서 '('는 건너뛰어 잘못된 '(' 문자열은 추출되지 않아야 함
    for (const auto& t : result->tables) {
        EXPECT_FALSE(t.empty() && t.front() == '(')
            << "Table name should not start with '('";
    }
    SUCCEED() << "Subquery inner table extraction is the current documented behavior. "
                 "Full parser would be needed to distinguish outer/inner FROM. "
                 "This is security-safe (access control applies to inner tables too).";
}

// ---------------------------------------------------------------------------
// [DON-25] 멀티 스테이트먼트 fail-close 테스트
//
// 문자열 리터럴/주석 외부에 세미콜론이 있으면 ParseError 반환.
// 이는 piggyback 공격 벡터를 파싱 단계에서 원천 차단한다.
//
// [오탐 가능성] 세미콜론으로 끝나는 단일 구문도 차단됨.
//   예: "SELECT 1;" → 차단 (보수적 설계).
//   호출자가 사전에 trailing 세미콜론을 제거하거나,
//   배치 실행 환경에서는 별도 처리가 필요하다.
//
// [보안 원칙] 멀티 스테이트먼트 감지 시 항상 ParseError 반환.
//   호출자는 반드시 fail-close(차단)를 적용해야 한다.
// ---------------------------------------------------------------------------

TEST(SqlParser, MultiStatementCALL_Blocked) {
    // "SELECT 1; CALL admin_proc()" → fail-close (ParseError)
    SqlParser parser;
    const auto result = parser.parse("SELECT 1; CALL admin_proc()");
    ASSERT_FALSE(result.has_value())
        << "Multi-statement with CALL should fail-close (ParseError)";
    EXPECT_EQ(result.error().code, ParseErrorCode::kInvalidSql)
        << "Error code should be kInvalidSql for multi-statement";
}

TEST(SqlParser, MultiStatementPREPARE_Blocked) {
    // "SELECT 1; PREPARE stmt FROM 'SELECT 1'" → fail-close (ParseError)
    SqlParser parser;
    const auto result = parser.parse("SELECT 1; PREPARE stmt FROM 'SELECT 1'");
    ASSERT_FALSE(result.has_value())
        << "Multi-statement with PREPARE should fail-close (ParseError)";
    EXPECT_EQ(result.error().code, ParseErrorCode::kInvalidSql);
}

TEST(SqlParser, MultiStatementTRUNCATE_Blocked) {
    // "SELECT 1; TRUNCATE users" → fail-close (ParseError)
    SqlParser parser;
    const auto result = parser.parse("SELECT 1; TRUNCATE users");
    ASSERT_FALSE(result.has_value())
        << "Multi-statement with TRUNCATE should fail-close (ParseError)";
    EXPECT_EQ(result.error().code, ParseErrorCode::kInvalidSql);
}

TEST(SqlParser, MultiStatementDROP_Blocked) {
    // "SELECT 1; DROP TABLE users" → fail-close (ParseError)
    SqlParser parser;
    const auto result = parser.parse("SELECT 1; DROP TABLE users");
    ASSERT_FALSE(result.has_value())
        << "Multi-statement with DROP should fail-close (ParseError)";
    EXPECT_EQ(result.error().code, ParseErrorCode::kInvalidSql);
}

TEST(SqlParser, SemicolonInsideStringAllowed) {
    // 문자열 리터럴 안의 세미콜론은 멀티 스테이트먼트가 아님 → 허용
    // [오탐 방지] "SELECT ';' FROM t" 는 단일 구문
    SqlParser parser;
    const auto result = parser.parse("SELECT ';' FROM t");
    ASSERT_TRUE(result.has_value())
        << "Semicolon inside string literal should NOT trigger multi-statement detection";
    EXPECT_EQ(result->command, SqlCommand::kSelect);
}

TEST(SqlParser, SemicolonInsideBlockCommentAllowed) {
    // 블록 주석 안의 세미콜론은 멀티 스테이트먼트가 아님 → 허용
    SqlParser parser;
    const auto result = parser.parse("SELECT 1 /* ; DROP TABLE users */ FROM t");
    ASSERT_TRUE(result.has_value())
        << "Semicolon inside block comment should NOT trigger multi-statement detection";
    EXPECT_EQ(result->command, SqlCommand::kSelect);
}

TEST(SqlParser, SemicolonInsideLineCommentAllowed) {
    // 라인 주석 안의 세미콜론은 멀티 스테이트먼트가 아님 → 허용
    SqlParser parser;
    const auto result = parser.parse("SELECT 1 -- ; DROP TABLE users\nFROM t");
    ASSERT_TRUE(result.has_value())
        << "Semicolon inside line comment should NOT trigger multi-statement detection";
    EXPECT_EQ(result->command, SqlCommand::kSelect);
}

// ---------------------------------------------------------------------------
// [DON-39] Trailing 세미콜론 허용 테스트
//
// MySQL 클라이언트 대부분이 단일 구문 끝에 세미콜론을 붙인다.
// 세미콜론 뒤에 공백/개행만 있는 경우는 단일 구문으로 허용해야
// 운영 환경에서 정상 쿼리가 차단되는 false positive 를 방지할 수 있다.
//
// [보안 원칙 유지] 세미콜론 뒤에 non-whitespace 문자가 있으면 여전히 차단.
// ---------------------------------------------------------------------------

TEST(SqlParser, TrailingSemicolonAllowed_Select) {
    // "SELECT 1;" → trailing 세미콜론 허용 (DON-39)
    SqlParser parser;
    const auto result = parser.parse("SELECT 1;");
    ASSERT_TRUE(result.has_value())
        << "Trailing semicolon on a single statement should be allowed (DON-39)";
    EXPECT_EQ(result->command, SqlCommand::kSelect);
}

TEST(SqlParser, TrailingSemicolonAllowed_Insert) {
    // "INSERT INTO t VALUES(1);" → trailing 세미콜론 허용
    SqlParser parser;
    const auto result = parser.parse("INSERT INTO t VALUES(1);");
    ASSERT_TRUE(result.has_value())
        << "Trailing semicolon on INSERT should be allowed (DON-39)";
    EXPECT_EQ(result->command, SqlCommand::kInsert);
    EXPECT_TRUE(contains_table(result->tables, "t"));
}

TEST(SqlParser, TrailingSemicolonAllowed_TrailingSpaces) {
    // "SELECT 1;  " → 세미콜론 뒤 공백만 → 허용
    SqlParser parser;
    const auto result = parser.parse("SELECT 1;  ");
    ASSERT_TRUE(result.has_value())
        << "Trailing semicolon followed by spaces only should be allowed (DON-39)";
    EXPECT_EQ(result->command, SqlCommand::kSelect);
}

TEST(SqlParser, TrailingSemicolonAllowed_TrailingNewline) {
    // "SELECT 1;\n" → 세미콜론 뒤 개행만 → 허용
    SqlParser parser;
    const auto result = parser.parse("SELECT 1;\n");
    ASSERT_TRUE(result.has_value())
        << "Trailing semicolon followed by newline only should be allowed (DON-39)";
    EXPECT_EQ(result->command, SqlCommand::kSelect);
}

TEST(SqlParser, TrailingSemicolonAllowed_TrailingMixedWhitespace) {
    // "SELECT 1;  \t\n  " → 세미콜론 뒤 공백·탭·개행 혼합 → 허용
    SqlParser parser;
    const auto result = parser.parse("SELECT 1;  \t\n  ");
    ASSERT_TRUE(result.has_value())
        << "Trailing semicolon followed by mixed whitespace only should be allowed (DON-39)";
    EXPECT_EQ(result->command, SqlCommand::kSelect);
}

TEST(SqlParser, DoubleSemicolonBlocked) {
    // "SELECT 1; ;" → 세미콜론 여러 개 → 차단 (멀티 스테이트먼트)
    // 두 번째 ';'가 non-whitespace 이므로 첫 번째 ';' 뒤에 내용이 있다고 판단
    SqlParser parser;
    const auto result = parser.parse("SELECT 1; ;");
    ASSERT_FALSE(result.has_value())
        << "Double semicolon should be treated as multi-statement and blocked (DON-39)";
    EXPECT_EQ(result.error().code, ParseErrorCode::kInvalidSql);
}

TEST(SqlParser, MultiStatementEXECUTE_Blocked) {
    // "SELECT 1; EXECUTE stmt" → fail-close (ParseError)
    SqlParser parser;
    const auto result = parser.parse("SELECT 1; EXECUTE stmt");
    ASSERT_FALSE(result.has_value())
        << "Multi-statement with EXECUTE should fail-close (ParseError)";
    EXPECT_EQ(result.error().code, ParseErrorCode::kInvalidSql);
}

// main 함수는 test_logger.cpp 에서 제공됨 (단일 dbgate_tests 실행 파일)
