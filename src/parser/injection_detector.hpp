#pragma once

// ---------------------------------------------------------------------------
// injection_detector.hpp
//
// 정규식 패턴 기반 SQL Injection 탐지기.
// 독립적 모듈 — 외부 헤더 의존성을 최소화한다.
//
// [탐지 대상 패턴 (기본값, config에서 로드)]
// - ' OR 1=1 --  (Boolean-based blind injection)
// - UNION SELECT  (UNION-based injection)
// - SLEEP()       (Time-based blind injection)
// - BENCHMARK()   (Time-based blind injection)
// - PREPARE/EXECUTE 를 통한 동적 SQL 우회 (procedure_detector 와 조합 권장)
//
// [설계 한계 / 알려진 우회 가능성]
// 1. 주석 분할:  UN/**/ION SEL/**/ECT 는 기본 패턴으로 탐지 불가.
//    → 전처리 단계에서 주석 제거를 수행해야 하나 현재 미구현.
// 2. 대소문자 변형: 패턴 매칭 시 case-insensitive 플래그 사용하여 완화.
// 3. 공백 변형: 탭·줄바꿈 치환 전처리는 부분 적용 (개선 여지 있음).
// 4. 인코딩 우회: URL 인코딩, hex 리터럴 (0x44524f50='DROP') 탐지 불가.
// 5. 2차 인젝션: 저장된 데이터를 통한 간접 공격은 네트워크 레이어에서
//    탐지 불가 (애플리케이션 레이어 책임).
//
// [오탐/미탐 트레이드오프]
// - 패턴을 넓힐수록: ORM 생성 쿼리(예: UNION 포함 페이징)에서
//   false positive 증가. 운영 환경에서 서비스 장애 유발 가능.
// - 패턴을 좁힐수록: 변형된 공격에서 false negative 증가.
// - 패턴은 config에서 로드하여 운영자가 튜닝 가능하도록 설계.
//
// [보안 원칙]
// - InjectionResult::detected == true 이면 정책 엔진은 kBlock 을 권고.
// - 탐지 불확실 시에도 detected == false 로 반환하며, 정책 엔진의
//   다른 규칙이 추가 필터링을 담당한다.
// ---------------------------------------------------------------------------

#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// InjectionResult
//   SQL Injection 탐지 결과.
//   matched_pattern 은 로깅/감사 목적으로만 사용하며, 클라이언트에
//   노출하지 않는다 (공격자 피드백 최소화).
// ---------------------------------------------------------------------------
struct InjectionResult {
    bool        detected{false};        // true = 인젝션 패턴 탐지됨
    std::string matched_pattern{};      // 매칭된 정규식 패턴 (감사 로그용)
    std::string reason{};               // 사람이 읽을 수 있는 탐지 이유
};

// ---------------------------------------------------------------------------
// InjectionDetector
//   생성 시 패턴 목록을 받아 정규식으로 컴파일하고, check() 에서 매칭.
//
//   [성능 고려사항]
//   - 생성자에서 std::regex 컴파일 비용이 발생하므로 인스턴스를 재사용할 것.
//   - 패턴 수와 SQL 길이에 비례하여 O(P * N) 복잡도. 과도한 패턴 수는
//     레이턴시를 증가시킨다.
//   - 입력 길이 제한은 호출자(proxy 레이어)가 사전에 적용해야 한다.
//
//   [우회 주의]
//   - 패턴 목록이 비어 있으면 모든 SQL 이 detected=false 로 통과한다.
//     빈 패턴 목록을 허용하지 않으려면 config 검증에서 걸러야 한다.
// ---------------------------------------------------------------------------
class InjectionDetector {
public:
    // 생성자: 패턴 목록을 받아 std::regex 로 컴파일한다.
    // patterns: 정규식 문자열 목록 (config/policy.yaml 에서 로드)
    //   - 각 패턴은 POSIX ERE 또는 ECMAScript regex 문법을 사용한다.
    //   - 잘못된 패턴은 생성자에서 로깅 후 건너뛴다 (fail-open 방지:
    //     유효한 나머지 패턴은 계속 적용됨).
    explicit InjectionDetector(std::vector<std::string> patterns);

    ~InjectionDetector() = default;

    // 복사 금지 (컴파일된 regex 재사용 비용 방지), 이동 허용
    InjectionDetector(const InjectionDetector&)            = delete;
    InjectionDetector& operator=(const InjectionDetector&) = delete;
    InjectionDetector(InjectionDetector&&)                 = default;
    InjectionDetector& operator=(InjectionDetector&&)      = default;

    // check
    //   sql: 원문 SQL 또는 전처리된 SQL
    //   반환: InjectionResult
    //
    // [오탐 주의]
    // - UNION 을 포함하는 합법적 쿼리(예: 페이징용 UNION ALL)에서
    //   false positive 발생 가능. 패턴 튜닝으로 완화해야 한다.
    // [미탐 주의]
    // - UN/**/ION 등 주석 분할 우회는 현재 탐지하지 못한다.
    [[nodiscard]] InjectionResult check(std::string_view sql) const;

private:
    // 컴파일된 정규식과 원본 패턴 문자열을 쌍으로 보관.
    // 구현 파일에서 std::regex 를 포함하므로 헤더에서는 전방 선언만 사용.
    struct CompiledPattern;
    std::vector<CompiledPattern> compiled_patterns_;
};
