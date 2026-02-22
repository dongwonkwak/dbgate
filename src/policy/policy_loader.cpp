// ---------------------------------------------------------------------------
// policy_loader.cpp — stub 구현
//
// [Phase 2 상태] 인터페이스 정의 완료, YAML 파싱 로직 미구현.
// Phase 3 에서 yaml-cpp 기반 PolicyConfig 파싱 구현 예정.
//
// [fail-close 보장]
// load() 는 항상 std::unexpected(error_message) 를 반환한다.
// 호출자는 실패 시 반드시 이전 정책 유지 또는 서비스 차단 처리를 해야 한다.
//
// [Phase 3 구현 시 주의사항]
// - 부분 파싱된 PolicyConfig 를 반환하지 말 것 (all-or-nothing).
// - regex 패턴 검증 실패 시 해당 패턴을 건너뛰고 경고 로그 출력.
// - 파일 경로 정규화(절대 경로 변환)를 수행하여 path traversal 방지.
// - YAML 파일 전체를 로그에 출력하지 말 것 (민감 정보 포함 가능).
//
// [watch() — Phase 4 예정]
// inotify 또는 Boost.Asio timer 기반 파일 감시.
// 파싱 실패 시 콜백 미호출 (기존 정책 유지, fail-close).
// ---------------------------------------------------------------------------

#include "policy/policy_loader.hpp"

std::expected<PolicyConfig, std::string>
PolicyLoader::load(const std::filesystem::path& /*config_path*/) {
    // stub: Phase 3 에서 yaml-cpp 기반 구현 예정.
    // fail-close: 미구현 → 항상 실패 반환.
    // 호출자는 이 실패를 받으면 기존 정책 유지 또는 모든 접근 차단.
    return std::unexpected<std::string>(
        "PolicyLoader::load not implemented"
    );
}
