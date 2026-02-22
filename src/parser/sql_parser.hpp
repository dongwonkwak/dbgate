#pragma once

// ---------------------------------------------------------------------------
// sql_parser.hpp
//
// SQL 구문 분류 및 테이블명 추출을 담당하는 "첫 번째 키워드 기반 분류 +
// 정규식 패턴 매칭" 수준의 경량 파서.
//
// [설계 한계 / 알려진 우회 가능성]
// 1. 주석 내 SQL 우회: DROP/**/TABLE, /*!DROP*/ 같은 인라인 주석 분할은
//    전처리 없이 놓칠 수 있다.
// 2. 인코딩 우회: URL 인코딩, 멀티바이트 문자 경계 조작은 탐지하지 못한다.
// 3. 복잡한 서브쿼리: SELECT * FROM (SELECT ...) AS t 에서 내부 테이블명은
//    추출하지 않는다.
// 4. 대소문자/공백 변형: 전처리(대문자 변환)는 수행하나, 탭·줄바꿈 혼합 시
//    일부 패턴을 놓칠 수 있다.
// 5. PREPARE/EXECUTE로 숨겨진 동적 SQL: 문자열 리터럴 내부까지 파싱하지
//    않으므로 procedure_detector 와 조합하여 탐지해야 한다.
//
// [오탐/미탐 트레이드오프]
// - 탐지 범위를 넓힐수록 ORM 생성 쿼리에서 false positive 증가한다.
// - 탐지 범위를 좁힐수록 우회 공격에서 false negative 증가한다.
// - 현재 구현은 보수적(차단 우선) 기본값을 유지한다.
// ---------------------------------------------------------------------------

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.hpp"  // ParseError, ParseErrorCode

// ---------------------------------------------------------------------------
// SqlCommand
//   SQL 문의 첫 번째 키워드 기반 분류.
//   kUnknown 은 분류 실패를 나타내며, 정책 엔진에서 fail-close 처리한다.
// ---------------------------------------------------------------------------
enum class SqlCommand : std::uint8_t {
    kSelect   = 0,
    kInsert   = 1,
    kUpdate   = 2,
    kDelete   = 3,
    kDrop     = 4,
    kTruncate = 5,
    kAlter    = 6,
    kCreate   = 7,
    kCall     = 8,
    kPrepare  = 9,
    kExecute  = 10,
    kUnknown  = 11,  // 분류 불가 — 정책 엔진에서 kBlock 으로 처리됨
};

// ---------------------------------------------------------------------------
// ParsedQuery
//   파싱 성공 시 반환되는 SQL 분석 결과.
//   raw_sql 은 원문 그대로 보존하며, 로깅/감사 목적으로만 사용한다.
// ---------------------------------------------------------------------------
struct ParsedQuery {
    SqlCommand               command{SqlCommand::kUnknown};
    std::vector<std::string> tables{};           // FROM/INTO/UPDATE/JOIN 뒤 테이블명
    std::string              raw_sql{};          // 원문 SQL (로깅용, 변형 없음)
    bool                     has_where_clause{}; // DELETE 무조건 삭제 탐지용
};

// ---------------------------------------------------------------------------
// SqlParser
//   SQL 문자열을 받아 ParsedQuery 로 변환한다.
//   실패 시 std::unexpected(ParseError) 를 반환한다.
//
//   [파서 보안 원칙]
//   - 파싱 실패는 절대 kAllow 로 이어지지 않는다. 호출자는 error path 에서
//     반드시 PolicyEngine::evaluate_error 를 통해 kBlock 을 적용해야 한다.
// ---------------------------------------------------------------------------
class SqlParser {
public:
    SqlParser()  = default;
    ~SqlParser() = default;

    // 복사/이동 허용 (stateless)
    SqlParser(const SqlParser&)            = default;
    SqlParser& operator=(const SqlParser&) = default;
    SqlParser(SqlParser&&)                 = default;
    SqlParser& operator=(SqlParser&&)      = default;

    // parse
    //   sql: 원문 SQL (null-terminated 불필요, view 로 전달)
    //   반환: ParsedQuery 또는 ParseError
    //
    // [오탐 주의]
    // ORM(예: Hibernate, SQLAlchemy)이 생성하는 복잡한 SELECT 에서
    // 테이블명 추출이 부정확할 수 있다. tables 벡터가 비어있더라도
    // 파싱 성공 자체는 유효하다.
    [[nodiscard]] std::expected<ParsedQuery, ParseError>
    parse(std::string_view sql) const;
};
