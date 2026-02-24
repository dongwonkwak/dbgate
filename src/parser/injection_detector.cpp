// ---------------------------------------------------------------------------
// injection_detector.cpp
//
// 정규식 패턴 기반 SQL Injection 탐지기 구현.
//
// [탐지 패턴 — 기본 10가지]
//  1. UNION\s+SELECT           — UNION 기반 인젝션
//  2. '\s*OR\s+['"\d]          — tautology (OR 기반 Boolean blind)
//  3. SLEEP\s*\(               — time-based blind
//  4. BENCHMARK\s*\(           — time-based blind
//  5. LOAD_FILE\s*\(           — 파일 읽기
//  6. INTO\s+OUTFILE            — 파일 쓰기
//  7. INTO\s+DUMPFILE           — 파일 덤프
//  8. ;\s*(DROP|DELETE|UPDATE|INSERT|ALTER|CREATE) — piggyback 공격
//  9. --\s*$                    — 주석 꼬리 무력화
// 10. /\*.*\*/                  — 인라인 주석 우회
//
// [오탐/미탐 트레이드오프]
// - 패턴 1: UNION SELECT 로 합법적인 UNION ALL 페이징 쿼리에서
//   false positive 발생 가능.
// - 패턴 2: OR 조건은 정상 쿼리에서도 사용. 패턴을 따옴표/숫자 시작으로
//   제한하여 false positive를 줄이나 다른 형태(OR true 등) 미탐.
// - 패턴 9 (-- 주석): 일부 MySQL 클라이언트 도구가 -- 주석을 사용할 수 있어
//   false positive 가능.
// - 패턴 10 (인라인 주석): 합법적인 /* */ 주석 포함 쿼리에서 false positive 가능.
//
// [알려진 우회 가능성]
// - UN/**/ION SEL/**/ECT 같은 주석 분할: 전처리 단계에서 주석 제거 없이
//   탐지 불가 (false negative).
// - 인코딩 우회: URL 인코딩, hex 리터럴은 탐지 불가 (false negative).
// - 빈 패턴 목록: 모든 SQL이 detected=false로 통과 (config 검증 필요).
//
// [CompiledPattern 구현 주의사항]
// InjectionDetector 헤더에서 ~InjectionDetector() = default 가 선언되어 있으므로
// vector<CompiledPattern>의 소멸자가 헤더 인스턴스화 지점에서 CompiledPattern의
// 완전한 정의를 요구한다. 이를 해결하기 위해 CompiledPattern 내부에서
// std::regex를 shared_ptr<std::regex>로 보관한다.
// shared_ptr은 incomplete type에 대해 소멸자를 타입-소거(type-erasure)하므로
// 헤더 포함 시점에서 완전한 정의가 없어도 동작한다.
// ---------------------------------------------------------------------------

#include "parser/injection_detector.hpp"

#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

// ---------------------------------------------------------------------------
// CompiledPattern: 헤더에서 전방 선언한 내부 구조체
//
// std::regex를 shared_ptr로 보관하는 이유:
//   헤더에 ~InjectionDetector() = default 가 선언되어 있어
//   vector<CompiledPattern>의 소멸자가 헤더 컴파일 시점에 인스턴스화된다.
//   CompiledPattern이 incomplete type인 상태에서 소멸자를 인스턴스화하면
//   컴파일 에러가 발생하므로, shared_ptr<regex>를 사용하여 소멸자를
//   type-erasure로 처리한다.
// ---------------------------------------------------------------------------
struct InjectionDetector::CompiledPattern {
    std::string                   source_pattern;  // 원본 패턴 문자열 (감사 로그용)
    std::shared_ptr<std::regex>   compiled;        // 컴파일된 정규식
    std::string                   reason;          // 사람이 읽을 수 있는 탐지 이유

    // CompiledPattern의 소멸자는 여기서 완전하게 정의됨.
    // shared_ptr<regex>의 소멸자는 이 시점에서 완전한 regex 정의를 가진다.
    ~CompiledPattern() = default;
    CompiledPattern()  = default;

    CompiledPattern(std::string src, std::shared_ptr<std::regex> re, std::string rsn)
        : source_pattern(std::move(src))
        , compiled(std::move(re))
        , reason(std::move(rsn))
    {}

    // 이동 지원
    CompiledPattern(CompiledPattern&&)            = default;
    CompiledPattern& operator=(CompiledPattern&&) = default;

    // 복사 지원 (shared_ptr 공유)
    CompiledPattern(const CompiledPattern&)            = default;
    CompiledPattern& operator=(const CompiledPattern&) = default;
};

// ---------------------------------------------------------------------------
// InjectionDetector 소멸자
// cpp에서 정의하는 이유: CompiledPattern의 완전한 정의 이후에 소멸자가 인스턴스화되어야
// vector<CompiledPattern>의 소멸자가 올바르게 컴파일된다.
// 동작: default 소멸자와 동일.
// ---------------------------------------------------------------------------
InjectionDetector::~InjectionDetector() = default;

// ---------------------------------------------------------------------------
// InjectionDetector 생성자
// ---------------------------------------------------------------------------
InjectionDetector::InjectionDetector(std::vector<std::string> patterns) {
    compiled_patterns_.reserve(patterns.size());

    for (auto& p : patterns) {
        try {
            auto re = std::make_shared<std::regex>(
                p,
                std::regex_constants::icase | std::regex_constants::ECMAScript
            );
            CompiledPattern cp(p, std::move(re), "Matched injection pattern: " + p);
            compiled_patterns_.push_back(std::move(cp));

        } catch (const std::regex_error& e) {
            // 잘못된 정규식은 로그 후 건너뜀.
            // [보안 주의] 잘못된 패턴을 건너뛰면 탐지 범위가 줄어든다 (false negative 증가).
            // fail-open을 방지하기 위해 유효한 나머지 패턴은 계속 적용한다.
            spdlog::warn(
                "injection_detector: invalid regex pattern '{}', skipping: {}",
                p, e.what()
            );
        }
    }

    // [Fail-close 보장] 유효한 패턴이 하나도 없으면 fail_close_active_ 를 true 로 설정.
    // 이 상태에서 check() 는 항상 detected=true 를 반환하여 모든 SQL 을 차단한다.
    //
    // [트레이드오프]
    // - false positive: 모든 SQL 이 차단되어 서비스 중단 가능.
    // - 하지만 패턴 없이 탐지를 허용하면 인젝션 공격이 무조건 통과 (false negative).
    // - 보안 우선 원칙에 따라 fail-close 를 선택한다.
    if (compiled_patterns_.empty()) {
        fail_close_active_ = true;
        spdlog::error(
            "injection_detector: no valid injection patterns loaded, "
            "fail-close active — all SQL will be blocked"
        );
    }
}

// ---------------------------------------------------------------------------
// InjectionDetector::check 구현
// ---------------------------------------------------------------------------
InjectionResult InjectionDetector::check(std::string_view sql) const {
    // [Fail-close] 유효한 패턴이 없으면 모든 SQL 을 차단.
    // 패턴 없이 탐지를 허용하면 인젝션 우회가 무조건 성공하므로
    // 운영자에게 설정 오류를 알리고 차단하는 것이 더 안전하다.
    if (fail_close_active_) {
        return InjectionResult{
            true,
            "",
            "no valid patterns loaded"
        };
    }

    const std::string sql_str(sql);

    for (const auto& cp : compiled_patterns_) {
        if (!cp.compiled) {
            continue;
        }
        if (std::regex_search(sql_str, *cp.compiled)) {
            // 첫 번째 매칭 시 즉시 반환
            return InjectionResult{
                true,
                cp.source_pattern,
                cp.reason
            };
        }
    }

    return InjectionResult{false, "", ""};
}
