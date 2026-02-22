// ---------------------------------------------------------------------------
// procedure_detector.cpp — stub 구현
//
// [Phase 2 상태] 인터페이스 정의 완료, 로직 미구현.
// Phase 3 에서 CALL/PREPARE/EXECUTE/CREATE|ALTER|DROP PROCEDURE 탐지 구현.
//
// [미구현 시 동작]
// detect() 는 std::nullopt 를 반환한다.
// → 정책 엔진은 프로시저 정보 없이 다른 규칙으로만 판정한다.
// → block_dynamic_sql=true 설정이어도 stub 상태에서는 탐지 불가.
//   Phase 3 완료 전까지 동적 SQL 우회 위험 존재 (알려진 미탐).
//
// [구현 시 주의사항]
// - 변수 간접 참조(SET @q='DROP...'; PREPARE s FROM @q;)는
//   탐지 불가 — 주석과 테스트 케이스에 명시할 것.
// ---------------------------------------------------------------------------

#include "parser/procedure_detector.hpp"

std::optional<ProcedureInfo>
ProcedureDetector::detect(const ParsedQuery& /*query*/) const {
    // stub: Phase 3 에서 구현 예정.
    // 미구현 → nullopt 반환.
    // [보안 경고] 동적 SQL 우회(PREPARE/EXECUTE) 탐지 불가 상태.
    return std::nullopt;
}
