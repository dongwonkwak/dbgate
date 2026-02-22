// ---------------------------------------------------------------------------
// sql_parser.cpp — stub 구현
//
// [Phase 2 상태] 인터페이스 정의 완료, 로직 미구현.
// Phase 3 에서 첫 번째 키워드 기반 분류 + 정규식 테이블명 추출 구현 예정.
//
// [fail-close 보장]
// 미구현 상태에서 parse() 는 항상 kInternalError 를 반환한다.
// 호출자(proxy 레이어)는 std::unexpected 수신 시 반드시
// PolicyEngine::evaluate_error() 를 통해 kBlock 처리해야 한다.
//
// [알려진 한계 — 구현 후에도 유지]
// - 주석 분할 우회(DROP/**/TABLE)는 전처리 없이 탐지 불가.
// - PREPARE/EXECUTE 내부 문자열은 파싱하지 않음.
// - 복잡한 서브쿼리의 내부 테이블명 미추출.
// ---------------------------------------------------------------------------

#include "parser/sql_parser.hpp"

std::expected<ParsedQuery, ParseError>
SqlParser::parse(std::string_view /*sql*/) const {
    // stub: Phase 3 에서 구현 예정.
    // fail-close: 미구현 → kInternalError 반환.
    // 호출자는 반드시 evaluate_error() 로 kBlock 처리할 것.
    return std::unexpected(ParseError{
        ParseErrorCode::kInternalError,
        "SqlParser::parse not implemented",
        ""
    });
}
