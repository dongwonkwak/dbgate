#pragma once

// ---------------------------------------------------------------------------
// policy_loader.hpp
//
// YAML 정책 파일을 로드하고, 선택적으로 파일 변경을 감시하여 Hot Reload
// 콜백을 호출하는 로더.
//
// [설계 원칙]
// - load() 실패 시 std::unexpected(error_message) 반환. 호출자는
//   실패 시 반드시 기존 정책을 유지하거나 서비스를 차단해야 한다.
// - watch() (Phase 4): inotify 또는 Boost.Asio timer 기반 감시.
//   파일 파싱 실패 시 콜백 미호출 (기존 정책 유지, fail-close).
//
// [순환 의존성]
// policy_loader.hpp → rule.hpp (단방향만)
// ❌ rule.hpp → policy_loader.hpp 금지
//
// [보안 고려사항]
// - YAML 파일 경로는 config 에서만 지정하고 사용자 입력을 직접 사용 금지.
// - 파일 권한 검사는 OS 레이어 소관이나, 예상치 못한 경로 탐색(path
//   traversal) 방지를 위해 절대 경로 정규화를 권장한다.
// - 파싱 실패 원인은 로깅하되, YAML 파일 전체를 로그에 출력하지 말 것
//   (민감 정보 노출 방지).
// ---------------------------------------------------------------------------

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "rule.hpp"  // PolicyConfig

// ---------------------------------------------------------------------------
// PolicyLoader
//   정적 로드 + 선택적 Hot Reload 감시 기능을 제공한다.
//
//   [Phase 1 범위]
//   load() 만 필수 구현. watch() 는 Phase 4 에서 구현.
// ---------------------------------------------------------------------------
class PolicyLoader {
public:
    // ReloadCallback
    //   파일 변경 감지 후 새 정책 파싱 성공 시 호출되는 콜백.
    //   파싱 실패 시에는 콜백을 호출하지 않는다 (fail-close).
    using ReloadCallback = std::function<void(std::shared_ptr<PolicyConfig>)>;

    PolicyLoader()  = default;
    ~PolicyLoader() = default;

    PolicyLoader(const PolicyLoader&)            = default;
    PolicyLoader& operator=(const PolicyLoader&) = default;
    PolicyLoader(PolicyLoader&&)                 = default;
    PolicyLoader& operator=(PolicyLoader&&)      = default;

    // load
    //   지정된 경로의 YAML 파일을 읽어 PolicyConfig 로 파싱한다.
    //
    //   성공: std::expected<PolicyConfig, std::string> 의 value
    //   실패: std::unexpected(error_message)
    //         — 호출자는 실패 시 반드시 이전 정책 유지 또는 차단 처리.
    //
    //   [fail-close 요구사항]
    //   파일 없음, 파싱 오류, 스키마 불일치 모두 실패로 처리한다.
    //   부분적으로 파싱된 정책을 반환하지 않는다.
    //
    //   [오탐 주의]
    //   policy.yaml 의 regex 패턴이 잘못 작성되면 InjectionDetector 초기화
    //   시 패턴이 무시되어 false negative 가 발생한다. 로드 시 패턴 검증
    //   경고를 로그에 출력하도록 구현 레이어에서 처리할 것.
    [[nodiscard]] static std::expected<PolicyConfig, std::string>
    load(const std::filesystem::path& config_path);

    // watch (Phase 4 — 선택사항)
    //   config_path 파일의 변경을 비동기로 감시하고, 변경 감지 + 파싱 성공
    //   시 callback 을 호출한다.
    //
    //   [구현 가이드라인]
    //   - inotify (Linux) 또는 Boost.Asio timer + 파일 mtime 비교 방식 권장.
    //   - 파싱 실패 시 callback 미호출, 오류 로그 출력 후 감시 계속.
    //   - io_ctx 가 종료되면 감시도 중단된다.
    //
    //   [보안 주의]
    //   - 파일이 외부 공격자에 의해 교체될 위험을 고려하여, 로드 후
    //     정책 유효성 검사(최소 접근 제어 규칙 존재 여부 등)를 추가 권장.
    //
    // Phase 4 에서 Boost.Asio io_context 의존성 추가 예정.
    // 현재는 선언만 포함 (미구현).
    //
    // static void watch(
    //     const std::filesystem::path& config_path,
    //     boost::asio::io_context&     io_ctx,
    //     ReloadCallback               callback);
};
