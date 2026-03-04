#pragma once

// ---------------------------------------------------------------------------
// policy_version_store.hpp
//
// 정책 스냅샷 저장/로드/목록/정리를 담당하는 버전 스토어.
//
// [설계 원칙]
// - 원본 YAML 파일을 복사하여 스냅샷으로 저장한다 (YAML 재직렬화 없음).
// - SHA-256 해시(OpenSSL EVP)로 스냅샷 무결성을 추적한다.
// - max_versions_ 초과 시 오래된 스냅샷을 자동으로 삭제한다 (prune).
// - 모든 공개 메서드는 std::mutex 로 보호되어 스레드 안전하다.
//
// [fail-close 연계]
// - load_snapshot 실패(파일 없음, 파싱 오류) 시 std::unexpected 반환.
//   호출자는 반드시 기존 정책을 유지하거나 차단 처리해야 한다.
// - save_snapshot 실패는 데이터패스에 영향을 주지 않는다.
//   버전 저장 실패 = 스냅샷 미보관이며, 현재 동작 중인 정책은 그대로 유지.
//
// [파일명 패턴]
// v{version}_{iso_timestamp}.yaml
// 예: v1_20260304T103000Z.yaml
//
// [알려진 한계]
// - 스냅샷 디렉토리(.policy_versions/)가 외부에서 수정되면
//   versions_ 벡터와 실제 파일이 불일치할 수 있다.
//   재시작 시 생성자에서 디렉토리를 스캔하여 복원하므로 영속성은 유지된다.
// - compute_hash 실패 시 hash 필드가 빈 문자열로 저장된다 (스냅샷은 계속 저장).
// - SHA-256 해시는 무결성 확인용이며, 서명/인증은 범위 외.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rule.hpp"  // PolicyConfig

// ---------------------------------------------------------------------------
// PolicyVersionMeta
//   저장된 정책 스냅샷의 메타데이터.
//   version:       단조 증가 버전 번호 (1부터 시작).
//   timestamp:     ISO 8601 UTC 타임스탬프 (예: "20260304T103000Z").
//   rules_count:   해당 정책의 access_control 규칙 수.
//   hash:          원본 파일의 SHA-256 hex digest. 실패 시 빈 문자열.
//   snapshot_path: 저장된 스냅샷 파일의 절대 경로.
// ---------------------------------------------------------------------------
struct PolicyVersionMeta {
    std::uint64_t version{0};
    std::string timestamp{};
    std::uint32_t rules_count{0};
    std::string hash{};
    std::filesystem::path snapshot_path{};
};

// ---------------------------------------------------------------------------
// PolicyVersionStore
//   정책 스냅샷 저장/로드/목록/정리를 담당한다.
//
//   [스레드 안전성]
//   모든 공개 메서드는 std::mutex 로 직렬화된다.
//   save_snapshot 과 load_snapshot 을 동시에 호출해도 안전하다.
//
//   [생성자 동작]
//   versions_dir_ = config_dir / ".policy_versions"
//   디렉토리가 없으면 생성한다.
//   기존 스냅샷 파일을 스캔하여 versions_ 벡터를 복원한다.
//   파일명 패턴 매칭에 실패하거나 파싱 불가 파일은 무시한다.
// ---------------------------------------------------------------------------
class PolicyVersionStore {
public:
    // config_dir:    정책 설정 디렉토리 경로 (스냅샷 서브디렉토리 생성 위치).
    // max_versions:  유지할 최대 스냅샷 수. 초과 시 오래된 것부터 삭제.
    explicit PolicyVersionStore(const std::filesystem::path& config_dir,
                                std::uint32_t max_versions = 10);
    ~PolicyVersionStore() = default;

    // 복사/이동 금지 (mutex 멤버)
    PolicyVersionStore(const PolicyVersionStore&) = delete;
    PolicyVersionStore& operator=(const PolicyVersionStore&) = delete;
    PolicyVersionStore(PolicyVersionStore&&) = delete;
    PolicyVersionStore& operator=(PolicyVersionStore&&) = delete;

    // save_snapshot
    //   source_path 의 YAML 파일을 스냅샷 디렉토리에 복사하여 버전을 저장한다.
    //
    //   성공: PolicyVersionMeta 반환 (version, timestamp, hash 포함).
    //   실패: std::unexpected(error_message) 반환.
    //         실패해도 현재 동작 중인 정책/엔진에 영향 없음.
    //
    //   [보안 주의]
    //   source_path 는 신뢰된 경로만 전달해야 한다.
    //   path traversal 방지는 호출자 책임.
    [[nodiscard]] std::expected<PolicyVersionMeta, std::string> save_snapshot(
        const PolicyConfig& config, const std::filesystem::path& source_path);

    // load_snapshot
    //   version 에 해당하는 스냅샷을 파일에서 읽어 PolicyConfig 로 반환한다.
    //
    //   성공: PolicyConfig 반환.
    //   실패: std::unexpected(error_message) 반환.
    //         — 파일 없음, 파싱 오류, 버전 미발견 모두 실패.
    //
    //   [fail-close 연계]
    //   호출자는 실패 시 기존 정책을 유지하거나 차단 처리해야 한다.
    [[nodiscard]] std::expected<PolicyConfig, std::string> load_snapshot(
        std::uint64_t version) const;

    // list_versions
    //   저장된 모든 버전의 메타데이터를 반환한다 (최신 버전 먼저 정렬).
    [[nodiscard]] std::vector<PolicyVersionMeta> list_versions() const;

    // current_version
    //   마지막으로 저장된 버전 번호를 반환한다.
    //   저장된 버전이 없으면 0 을 반환한다.
    [[nodiscard]] std::uint64_t current_version() const;

private:
    std::filesystem::path versions_dir_;       // 스냅샷 저장 디렉토리
    std::uint32_t max_versions_;               // 유지할 최대 스냅샷 수
    mutable std::mutex mutex_;                 // 공개 메서드 직렬화용 뮤텍스
    std::uint64_t next_version_{1};            // 다음 버전 번호 (1부터 시작)
    std::vector<PolicyVersionMeta> versions_;  // 저장된 스냅샷 메타데이터 목록

    // prune
    //   versions_.size() > max_versions_ 이면 오래된 스냅샷 파일을 삭제하고
    //   versions_ 벡터에서 제거한다.
    //   mutex_ 가 이미 잠긴 상태에서 호출해야 한다 (내부 전용).
    void prune();

    // compute_hash
    //   file_path 의 내용에 대한 SHA-256 hex digest 문자열을 반환한다.
    //   파일 읽기 실패 또는 OpenSSL 오류 시 빈 문자열 반환.
    //
    //   [구현 노트]
    //   OpenSSL EVP_DigestInit_ex / EVP_DigestUpdate / EVP_DigestFinal_ex 사용.
    //   OpenSSL libcrypto 는 Boost.Asio SSL 의존으로 이미 vcpkg 에 설치됨.
    [[nodiscard]] static std::string compute_hash(const std::filesystem::path& file_path);
};
