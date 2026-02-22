// ---------------------------------------------------------------------------
// policy_engine.cpp — stub 구현
//
// [Phase 2 상태] 인터페이스 정의 완료, 정책 평가 로직 미구현.
// Phase 3 에서 실제 규칙 매칭 구현 예정.
//
// [fail-close 보장 — stub 에서도 반드시 준수]
// evaluate():       kBlock 반환 (default deny, 미구현)
// evaluate_error(): kBlock 반환 (파서 오류 → fail-close)
//
// ❌ 절대 금지: stub 이라도 kAllow 를 기본값으로 반환하지 말 것.
//
// [Phase 3 구현 예정 평가 순서]
// 1. SQL 구문 차단 (block_statements)
// 2. SQL 패턴 차단 (InjectionDetector)
// 3. 사용자/IP 접근 제어
// 4. 테이블 접근 제어
// 5. 시간대 제한
// 6. 프로시저 제어
// 7. 명시적 allow → kAllow
// 8. 일치 없음 → kBlock (default deny)
// ---------------------------------------------------------------------------

#include "policy/policy_engine.hpp"

PolicyEngine::PolicyEngine(std::shared_ptr<PolicyConfig> config)
    : config_(std::move(config)) {
    // config_ 가 nullptr 이면 이후 모든 evaluate() 가 kBlock 을 반환한다.
    // fail-close 원칙에 의해 nullptr config 도 허용하되, 차단으로 처리.
}

PolicyResult PolicyEngine::evaluate(
    const ParsedQuery&    /*query*/,
    const SessionContext& /*session*/) const {
    // stub: Phase 3 에서 구현 예정.
    // fail-close: 미구현 상태에서 모든 쿼리를 차단한다.
    // ❌ 이 반환값을 kAllow 로 변경하지 말 것.
    return PolicyResult{
        PolicyAction::kBlock,
        "stub",
        "PolicyEngine::evaluate not implemented — fail-close default"
    };
}

PolicyResult PolicyEngine::evaluate_error(
    const ParseError&     /*error*/,
    const SessionContext& /*session*/) const noexcept {
    // fail-close: 파서 오류 → 반드시 kBlock.
    // 이 함수는 어떠한 경우에도 kAllow 를 반환하지 않는다.
    return PolicyResult{
        PolicyAction::kBlock,
        "parse-error",
        "Parser error — fail-close"
    };
}

void PolicyEngine::reload(std::shared_ptr<PolicyConfig> new_config) {
    // stub: Phase 4 에서 atomic 교체 구현 예정.
    // 현재는 단순 대입. 스레드 안전성 미보장 (Phase 4 에서 개선).
    config_ = std::move(new_config);
}
