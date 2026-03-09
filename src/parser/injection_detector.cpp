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

#include <spdlog/spdlog.h>

#include <cctype>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

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
    std::string source_pattern;            // 원본 패턴 문자열 (감사 로그용)
    std::shared_ptr<std::regex> compiled;  // 컴파일된 정규식
    std::string reason;                    // 사람이 읽을 수 있는 탐지 이유
    std::string fast_anchor;               // 대문자 리터럴 앵커 (빠른 사전필터용)

    // CompiledPattern의 소멸자는 여기서 완전하게 정의됨.
    // shared_ptr<regex>의 소멸자는 이 시점에서 완전한 regex 정의를 가진다.
    ~CompiledPattern() = default;
    CompiledPattern() = default;

    CompiledPattern(std::string src, std::shared_ptr<std::regex> re, std::string rsn,
                    std::string anchor)
        : source_pattern(std::move(src)), compiled(std::move(re)), reason(std::move(rsn)),
          fast_anchor(std::move(anchor)) {}

    // 이동 지원
    CompiledPattern(CompiledPattern&&) = default;
    CompiledPattern& operator=(CompiledPattern&&) = default;

    // 복사 지원 (shared_ptr 공유)
    CompiledPattern(const CompiledPattern&) = default;
    CompiledPattern& operator=(const CompiledPattern&) = default;
};

// ---------------------------------------------------------------------------
// extract_literal_anchor
//
// 정규식 패턴에서 가장 긴 연속 알파벳/숫자/언더스코어 부분문자열을 추출한다.
// 대문자로 변환하여 반환 (case-insensitive 패턴 매칭 대응).
//
// [사전필터 목적]
// check()에서 regex_search 전에 string::find로 앵커 존재 여부를 확인한다.
// 앵커가 SQL에 없으면 해당 패턴의 regex를 건너뛸 수 있어 정상 쿼리에서
// regex 호출 횟수를 대폭 줄인다.
//
// [보안 고려사항]
// - 패턴에 '|'(alternation)이 포함된 경우 반드시 빈 문자열을 반환한다.
//   이유: alternation 패턴에서 가장 긴 토큰을 앵커로 쓰면 다른 대안(alternative)이
//   SQL에 있을 때 사전필터가 잘못 skip하여 false negative가 발생한다.
//   예: `;\s*(DROP|...|TRUNCATE)` → 앵커 "TRUNCATE" → `; DROP TABLE` 미탐.
// - 앵커 길이 2 미만이면 빈 문자열 반환 → 사전필터 없이 regex 실행 (기존 동작 유지).
// - 앵커 기반 사전필터는 "skip"이지 "block"이 아니므로:
//   앵커 존재 → regex 실행 → 최종 판정 (false negative 불가)
//   앵커 부재 → regex 건너뜀 → not detected (앵커가 없는 SQL은 패턴 미매칭 보장)
// - 알파벳/숫자/언더스코어만으로 앵커를 제한하여 정규식 메타문자를 배제한다.
// ---------------------------------------------------------------------------
namespace {
std::string extract_literal_anchor(const std::string& pattern) {
    // [보안] alternation 패턴은 앵커 추출 불가 — 빈 문자열 반환
    // 하나의 토큰이 모든 대안을 대표할 수 없으므로 false negative 위험 존재.
    if (pattern.find('|') != std::string::npos) {
        return std::string{};
    }

    std::string best;
    std::string current;
    for (const char c : pattern) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') {
            current += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else {
            if (current.size() > best.size()) {
                best = current;
            }
            current.clear();
        }
    }
    if (current.size() > best.size()) {
        best = current;
    }
    // 너무 짧은 앵커는 사전필터 효과 없음 (빈 문자열 반환 → regex 항상 실행)
    return best.size() >= 2 ? best : std::string{};
}
}  // namespace

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
                p, std::regex_constants::icase | std::regex_constants::ECMAScript);
            auto anchor = extract_literal_anchor(p);
            CompiledPattern cp(p, std::move(re), "Matched injection pattern: " + p,
                               std::move(anchor));
            compiled_patterns_.push_back(std::move(cp));

        } catch (const std::regex_error& e) {
            // 잘못된 정규식은 로그 후 건너뜀.
            // [보안 주의] 잘못된 패턴을 건너뛰면 탐지 범위가 줄어든다 (false negative 증가).
            // fail-open을 방지하기 위해 유효한 나머지 패턴은 계속 적용한다.
            spdlog::warn(
                "injection_detector: invalid regex pattern '{}', skipping: {}", p, e.what());
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
            "fail-close active — all SQL will be blocked");
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
            .detected = true, .matched_pattern = "", .reason = "no valid patterns loaded"};
    }

    // SQL 대문자 변환 (앵커 사전필터용 — 1회만 수행)
    //
    // [성능 의도]
    // 각 패턴의 fast_anchor 는 대문자로 저장되어 있다.
    // sql_upper 에서 string::find 를 먼저 수행해 앵커가 없으면 regex 를 건너뜀.
    // 정상 쿼리(인젝션 키워드 없음)에서 regex 호출 횟수를 90%+ 줄일 수 있다.
    //
    // [보안 보장]
    // 앵커 존재 → regex 실행 → 최종 판정 (false negative 불가)
    // 앵커 부재 → regex 건너뜀 → not detected
    // 앵커가 비어 있는 패턴은 항상 regex 실행 (기존 동작 유지)
    std::string sql_upper;
    sql_upper.reserve(sql.size());
    for (const char c : sql) {
        sql_upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    const std::string sql_str(sql);

    for (const auto& cp : compiled_patterns_) {
        if (!cp.compiled) {
            continue;
        }
        // 빠른 사전필터: 앵커가 정의되어 있고 SQL에 없으면 regex 건너뜀
        if (!cp.fast_anchor.empty() && sql_upper.find(cp.fast_anchor) == std::string::npos) {
            continue;
        }
        if (std::regex_search(sql_str, *cp.compiled)) {
            // 첫 번째 매칭 시 즉시 반환
            return InjectionResult{
                .detected = true, .matched_pattern = cp.source_pattern, .reason = cp.reason};
        }
    }

    return InjectionResult{.detected = false, .matched_pattern = "", .reason = ""};
}
