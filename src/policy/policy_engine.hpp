#pragma once

// ---------------------------------------------------------------------------
// policy_engine.hpp
//
// 파싱된 쿼리와 세션 컨텍스트를 받아 허용/차단/로깅 판정을 내리는 엔진.
//
// [fail-close 원칙 — 절대 위반 금지]
// 1. 파서 오류 발생 → evaluate_error() → 반드시 PolicyAction::kBlock
// 2. 정책 일치 없음 → 반드시 PolicyAction::kBlock (default deny)
// 3. 정책 엔진 내부 오류 → 반드시 PolicyAction::kBlock
// 4. PolicyAction::kAllow 는 명시적 허용 규칙이 존재할 때만 반환
//
// ❌ 금지: 파서 오류 → kAllow (fail-open)
// ❌ 금지: 정책 불확실 → kAllow (default whitelist)
// ❌ 금지: 예외 처리 중 kAllow 반환
//
// [Hot Reload]
// reload() 는 std::shared_ptr 교체를 통해 진행 중인 evaluate() 와
// 경쟁 없이 수행된다. C++ shared_ptr 의 atomic 특성에 의존하므로
// 구현 레이어에서 std::atomic<std::shared_ptr<PolicyConfig>> 사용 권장.
//
// [순환 의존성 — 무순환 구조]
// policy_engine.hpp → sql_parser.hpp  (단방향)
// policy_engine.hpp → rule.hpp        (단방향)
// policy_engine.hpp → common/types.hpp (단방향)
// ❌ rule.hpp → policy_engine.hpp 금지
// ❌ sql_parser.hpp → rule.hpp 금지
// ---------------------------------------------------------------------------

#include <atomic>
#include <memory>
#include <string>

#include "common/types.hpp"   // SessionContext, ParseError
#include "parser/sql_parser.hpp"  // ParsedQuery
#include "rule.hpp"               // PolicyConfig

// ---------------------------------------------------------------------------
// PolicyAction
//   정책 평가 결과 액션.
//   kLog: 허용하되 감사 로그 기록 (경보 수준)
// ---------------------------------------------------------------------------
enum class PolicyAction : std::uint8_t {
    kAllow = 0,  // 허용 (명시적 allow 규칙 일치 시만)
    kBlock = 1,  // 차단 (default deny 또는 명시적 block 규칙)
    kLog   = 2,  // 허용 + 감사 로그 (경보)
};

// ---------------------------------------------------------------------------
// PolicyResult
//   정책 평가 결과.
//   matched_rule: 어떤 규칙에 의해 판정됐는지 (감사 로그용).
//                 일치 규칙 없으면 "default-deny" 문자열.
//   reason: 사람이 읽을 수 있는 판정 이유 (로깅용).
//           클라이언트에 그대로 노출하지 말 것.
// ---------------------------------------------------------------------------
struct PolicyResult {
    PolicyAction action{PolicyAction::kBlock};  // 기본값 kBlock (fail-close)
    std::string  matched_rule{};                // 매칭된 규칙 식별자
    std::string  reason{};                      // 판정 이유 (감사 로그용)
};

// ---------------------------------------------------------------------------
// PolicyEngine
//   정책 설정을 기반으로 쿼리 허용/차단을 판정한다.
//
//   [스레드 안전성]
//   - evaluate/evaluate_error: 읽기 전용으로 concurrent 호출 안전.
//   - reload: shared_ptr 교체, 구현에서 atomic 교체 사용 권장.
// ---------------------------------------------------------------------------
class PolicyEngine {
public:
    // 생성자: 정책 설정을 주입받는다.
    // config 가 nullptr 이면 모든 evaluate() 가 kBlock 을 반환한다 (fail-close).
    explicit PolicyEngine(std::shared_ptr<PolicyConfig> config);

    ~PolicyEngine() = default;

    // 복사 금지 (shared_ptr 소유권 명확화), 이동 허용
    PolicyEngine(const PolicyEngine&)            = delete;
    PolicyEngine& operator=(const PolicyEngine&) = delete;
    PolicyEngine(PolicyEngine&&)                 = default;
    PolicyEngine& operator=(PolicyEngine&&)      = default;

    // evaluate
    //   파싱된 쿼리와 세션 컨텍스트를 기반으로 정책을 평가한다.
    //
    //   [평가 순서 (구현 레이어에서 반드시 준수)]
    //   1. SQL 구문 차단 규칙 (block_statements)
    //   2. SQL 패턴 차단 규칙 (block_patterns / InjectionDetector)
    //   3. 사용자/IP 접근 제어 (access_control)
    //   4. 테이블 접근 제어
    //   5. 시간대 제한
    //   6. 프로시저 제어
    //   7. 명시적 allow → kAllow 반환
    //   8. 일치 없음 → kBlock 반환 (default deny)
    //
    //   [오탐 주의]
    //   access_control 의 user 필드가 "*" 와일드카드일 때 모든 사용자에게
    //   적용되므로, 규칙 순서와 우선순위를 config 에서 명확히 정의해야 한다.
    [[nodiscard]] PolicyResult evaluate(
        const ParsedQuery&     query,
        const SessionContext&  session) const;

    // evaluate_error
    //   파서 오류 발생 시 호출. 반드시 PolicyAction::kBlock 을 반환한다.
    //
    //   [fail-close 보장]
    //   이 함수는 어떠한 경우에도 kAllow 또는 kLog 를 반환해서는 안 된다.
    //   구현 레이어에서 예외가 발생하더라도 kBlock 이 반환되도록 noexcept 권장.
    [[nodiscard]] PolicyResult evaluate_error(
        const ParseError&     error,
        const SessionContext& session) const noexcept;

    // reload
    //   Hot Reload: 새 정책 설정으로 원자적 교체.
    //   이미 진행 중인 evaluate() 는 이전 config 로 완료된다.
    //   new_config 가 nullptr 이면 이후 모든 evaluate() 가 kBlock 을 반환한다.
    void reload(std::shared_ptr<PolicyConfig> new_config);

private:
    // std::atomic<std::shared_ptr<PolicyConfig>> (C++20)
    // reload() 와 evaluate() 가 동시에 실행되는 경우에도 data race 없이
    // config 포인터를 원자적으로 교체/읽기할 수 있다.
    //
    // [설계 노트]
    // - evaluate() 에서는 load() 로 로컬 shared_ptr 을 취득한다.
    //   reload() 가 새 값으로 교체하더라도 로컬 복사본은 이전 config 의
    //   수명을 유지한다 (shared_ptr 참조 카운트 보장).
    // - reload() 에서는 store() 로 원자적 교체한다.
    // - public API(함수 시그니처) 는 변경 없음. private 멤버만 변경.
    std::atomic<std::shared_ptr<PolicyConfig>> config_;
};
