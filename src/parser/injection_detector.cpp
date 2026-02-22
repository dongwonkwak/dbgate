// ---------------------------------------------------------------------------
// injection_detector.cpp — stub 구현
//
// [Phase 2 상태] 인터페이스 정의 완료, regex 컴파일/매칭 미구현.
// Phase 3 에서 std::regex 기반 패턴 매칭 구현 예정.
//
// [미구현 시 동작]
// - 생성자: patterns 를 저장만 하고 컴파일하지 않는다.
// - check(): 항상 detected=false 반환.
//   → 모든 SQL 이 인젝션 탐지를 통과한다 (알려진 미탐).
//   → Phase 3 완료 전까지 인젝션 탐지 기능 비활성 상태.
//
// [구현 시 오탐/미탐 트레이드오프]
// - 패턴 추가 → ORM 생성 쿼리에서 false positive 위험 증가.
// - 패턴 제거/완화 → 변형 공격에서 false negative 위험 증가.
// - 주석 분할(UN/**/ION) 우회는 전처리 없이 탐지 불가 (알려진 한계).
// ---------------------------------------------------------------------------

#include "parser/injection_detector.hpp"

// CompiledPattern: 헤더에서 전방 선언한 내부 구조체.
// Phase 3 에서 std::regex 멤버 포함 예정.
struct InjectionDetector::CompiledPattern {
    std::string source_pattern;  // 원본 패턴 문자열 (감사 로그용)
    // Phase 3: std::regex compiled;
};

InjectionDetector::InjectionDetector(std::vector<std::string> patterns) {
    // stub: 패턴을 저장만 하고 컴파일하지 않는다.
    // Phase 3 에서 std::regex 컴파일 + 잘못된 패턴 로깅 처리 구현 예정.
    compiled_patterns_.reserve(patterns.size());
    for (auto& p : patterns) {
        compiled_patterns_.push_back(CompiledPattern{std::move(p)});
    }
}

InjectionResult InjectionDetector::check(std::string_view /*sql*/) const {
    // stub: Phase 3 에서 구현 예정.
    // 미구현 → detected=false 반환 (알려진 미탐 상태).
    // [보안 경고] 인젝션 탐지 비활성 — Phase 3 전까지 모든 패턴 미적용.
    return InjectionResult{
        false,
        "",
        "InjectionDetector::check not implemented"
    };
}
