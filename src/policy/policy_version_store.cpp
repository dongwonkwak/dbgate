// ---------------------------------------------------------------------------
// policy_version_store.cpp
//
// 정책 스냅샷 저장/로드/목록/정리 구현.
//
// [설계 원칙]
// - YAML 재직렬화 없이 원본 파일을 복사하여 스냅샷 저장.
// - OpenSSL EVP API 로 SHA-256 해시를 계산하여 무결성 추적.
// - std::mutex 로 공개 메서드를 직렬화하여 스레드 안전성 보장.
//
// [파일명 패턴]
// v{version}_{timestamp}.yaml
// 예: v1_20260304T103000Z.yaml
// 타임스탬프는 ISO 8601 UTC 형식 (YYYYMMDDTHHMMSSz).
//
// [스냅샷 디렉토리 스캔 (생성자)]
// 기존 스냅샷 파일을 정규식으로 파싱하여 versions_ 벡터를 복원한다.
// 파일명 파싱 실패 항목은 무시하고 로그 경고를 출력한다.
//
// [prune 동작]
// versions_ 는 오래된 것이 앞에 위치한다 (추가 순서 = 오래된 순서).
// prune() 은 앞에서부터 삭제하여 max_versions_ 개수를 유지한다.
//
// [알려진 한계]
// - 스냅샷 디렉토리 외부에서 파일이 추가/삭제되면 versions_ 와 불일치 발생.
//   재시작 시 생성자 스캔으로 복원됨.
// - compute_hash: 파일 읽기 실패 시 빈 문자열 반환 (스냅샷 저장은 계속).
// - prune 중 파일 삭제 실패 시 로그 경고만 출력하고 계속 진행.
// ---------------------------------------------------------------------------

#include "policy/policy_version_store.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <regex>
#include <span>
#include <sstream>
#include <string>
#include <vector>

// OpenSSL EVP for SHA-256
#include <openssl/evp.h>

#include "policy/policy_loader.hpp"

// ---------------------------------------------------------------------------
// 내부 헬퍼: 현재 UTC 시각을 ISO 8601 compact 형식으로 반환.
// 형식: YYYYMMDDTHHMMSSz (예: "20260304T103000Z")
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] std::string current_utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &time_t_now);
#else
    if (gmtime_r(&time_t_now, &utc_tm) == nullptr) {
        spdlog::warn("policy_version_store: gmtime_r failed, using empty timestamp");
        return {};
    }
#endif

    // YYYYMMDDTHHMMSSz 형식
    std::ostringstream oss;
    oss << std::put_time(&utc_tm, "%Y%m%dT%H%M%SZ");
    return oss.str();
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: 파일명에서 버전 번호와 타임스탬프를 파싱한다.
// 패턴: v{version}_{timestamp}.yaml
// 성공 시 {version, timestamp} 반환. 실패 시 {0, ""} 반환.
// ---------------------------------------------------------------------------
struct SnapshotFileParts {
    std::uint64_t version{0};
    std::string timestamp{};
};

[[nodiscard]] SnapshotFileParts parse_snapshot_filename(const std::string& stem) {
    // 파일명에서 확장자를 제거한 stem: "v1_20260304T103000Z"
    static const std::regex pattern(R"(^v(\d+)_(.+)$)");
    std::smatch m;
    if (!std::regex_match(stem, m, pattern)) {
        return {};
    }
    try {
        const std::uint64_t ver = std::stoull(m[1].str());
        SnapshotFileParts parts;
        parts.version = ver;
        parts.timestamp = m[2].str();
        return parts;
    } catch (...) {
        return {};
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// PolicyVersionStore 생성자
//
// 1. versions_dir_ = config_dir / ".policy_versions" 설정
// 2. 디렉토리가 없으면 생성
// 3. 기존 스냅샷 파일을 스캔하여 versions_ 복원
// ---------------------------------------------------------------------------
PolicyVersionStore::PolicyVersionStore(const std::filesystem::path& config_dir,
                                       std::uint32_t max_versions)
    : versions_dir_(config_dir / ".policy_versions"), max_versions_(max_versions) {
    // 디렉토리 생성 (이미 있으면 무시)
    std::error_code ec;
    std::filesystem::create_directories(versions_dir_, ec);
    if (ec) {
        spdlog::warn("policy_version_store: cannot create versions dir '{}': {}",
                     versions_dir_.string(),
                     ec.message());
        // 디렉토리 생성 실패해도 계속 진행 (save_snapshot 시 실패 처리)
    }

    // 기존 스냅샷 파일 스캔하여 versions_ 복원
    // 파일명 패턴: v{version}_{timestamp}.yaml
    std::vector<PolicyVersionMeta> found;

    std::error_code it_ec;
    for (const auto& entry : std::filesystem::directory_iterator(versions_dir_, it_ec)) {
        if (it_ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto& path = entry.path();
        if (path.extension() != ".yaml") {
            continue;
        }

        const auto stem = path.stem().string();
        const auto parts = parse_snapshot_filename(stem);
        if (parts.version == 0) {
            spdlog::warn("policy_version_store: skipping unrecognized snapshot file '{}'",
                         path.filename().string());
            continue;
        }

        PolicyVersionMeta meta;
        meta.version = parts.version;
        meta.timestamp = parts.timestamp;
        meta.snapshot_path = path;
        // rules_count 와 hash 는 스캔 시 0/"" 로 초기화
        // (재시작 시 정확한 값 복원 불가 — 알려진 한계)
        found.push_back(std::move(meta));
    }

    if (it_ec) {
        spdlog::warn("policy_version_store: directory iteration error for '{}': {}",
                     versions_dir_.string(),
                     it_ec.message());
    }

    // 버전 번호 오름차순 정렬 (오래된 것 먼저)
    std::ranges::sort(found, [](const PolicyVersionMeta& a, const PolicyVersionMeta& b) {
        return a.version < b.version;
    });

    versions_ = std::move(found);

    // next_version_ 은 현재 최대 버전 + 1
    if (!versions_.empty()) {
        next_version_ = versions_.back().version + 1;
    }

    spdlog::info(
        "policy_version_store: initialized with {} existing snapshot(s), "
        "next_version={}",
        versions_.size(),
        next_version_);
}

// ---------------------------------------------------------------------------
// save_snapshot
//
// 1. source_path 의 YAML 파일을 스냅샷 디렉토리에 복사
// 2. SHA-256 해시 계산 (원본 파일 기준)
// 3. PolicyVersionMeta 생성 및 versions_ 추가
// 4. prune() 호출
// 5. next_version_++ 후 메타 반환
// ---------------------------------------------------------------------------
std::expected<PolicyVersionMeta, std::string> PolicyVersionStore::save_snapshot(
    const PolicyConfig& config, const std::filesystem::path& source_path) {
    const std::lock_guard<std::mutex> lock(mutex_);

    // source_path 존재 확인
    std::error_code ec;
    if (!std::filesystem::exists(source_path, ec) || ec) {
        const std::string err = fmt::format(
            "policy_version_store: source file does not exist: '{}'", source_path.string());
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    // 타임스탬프 생성
    const std::string timestamp = current_utc_timestamp();

    // 스냅샷 파일명: v{version}_{timestamp}.yaml
    const std::string filename = fmt::format("v{}_{}.yaml", next_version_, timestamp);
    const auto snapshot_path = versions_dir_ / filename;

    // 원본 파일 복사
    std::filesystem::copy_file(
        source_path, snapshot_path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        const std::string err = fmt::format("policy_version_store: failed to copy '{}' to '{}': {}",
                                            source_path.string(),
                                            snapshot_path.string(),
                                            ec.message());
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    // SHA-256 해시 계산 (원본 파일 기준)
    const std::string hash = compute_hash(source_path);
    if (hash.empty()) {
        spdlog::warn(
            "policy_version_store: failed to compute SHA-256 for '{}' — hash will be empty",
            source_path.string());
    }

    // access_control 규칙 수 계산
    const auto rules_count = static_cast<std::uint32_t>(config.access_control.size());

    // 메타데이터 생성
    PolicyVersionMeta meta;
    meta.version = next_version_;
    meta.timestamp = timestamp;
    meta.rules_count = rules_count;
    meta.hash = hash;
    meta.snapshot_path = snapshot_path;

    versions_.push_back(meta);
    next_version_++;

    // 오래된 스냅샷 정리
    prune();

    spdlog::info("policy_version_store: saved snapshot v{} (rules={}, hash={:.16}...) → '{}'",
                 meta.version,
                 meta.rules_count,
                 hash.empty() ? "(none)" : hash,
                 snapshot_path.string());

    return meta;
}

// ---------------------------------------------------------------------------
// load_snapshot
//
// versions_ 에서 version 을 찾아 PolicyLoader::load() 로 파싱하여 반환.
// ---------------------------------------------------------------------------
std::expected<PolicyConfig, std::string> PolicyVersionStore::load_snapshot(
    std::uint64_t version) const {
    const std::lock_guard<std::mutex> lock(mutex_);

    // 버전 검색
    const auto it = std::ranges::find_if(versions_, [version](const PolicyVersionMeta& m) {
        return m.version == version;
    });

    if (it == versions_.end()) {
        const std::string err = fmt::format("policy_version_store: version {} not found", version);
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    // 파일 존재 확인
    std::error_code ec;
    if (!std::filesystem::exists(it->snapshot_path, ec) || ec) {
        const std::string err =
            fmt::format("policy_version_store: snapshot file missing for version {}: '{}'",
                        version,
                        it->snapshot_path.string());
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    // PolicyLoader 로 파싱
    auto result = PolicyLoader::load(it->snapshot_path);
    if (!result) {
        const std::string err = fmt::format(
            "policy_version_store: failed to parse snapshot v{}: {}", version, result.error());
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    spdlog::info(
        "policy_version_store: loaded snapshot v{} from '{}'", version, it->snapshot_path.string());
    return std::move(*result);
}

// ---------------------------------------------------------------------------
// list_versions
//
// versions_ 복사본을 최신 버전 먼저 정렬하여 반환.
// ---------------------------------------------------------------------------
std::vector<PolicyVersionMeta> PolicyVersionStore::list_versions() const {
    const std::lock_guard<std::mutex> lock(mutex_);

    // 내부 versions_ 는 오래된 것 먼저 — 복사 후 역순 정렬
    std::vector<PolicyVersionMeta> result = versions_;
    std::ranges::sort(result, [](const PolicyVersionMeta& a, const PolicyVersionMeta& b) {
        return a.version > b.version;  // 최신 먼저
    });
    return result;
}

// ---------------------------------------------------------------------------
// current_version
//
// 마지막으로 저장된 버전 번호를 반환.
// versions_ 가 비어있으면 0 반환.
// ---------------------------------------------------------------------------
std::uint64_t PolicyVersionStore::current_version() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (versions_.empty()) {
        return 0;
    }
    // versions_ 는 오래된 것 먼저 → 마지막 원소가 최신
    return versions_.back().version;
}

// ---------------------------------------------------------------------------
// prune (private, mutex_ 잠긴 상태에서 호출)
//
// versions_.size() > max_versions_ 이면 오래된 스냅샷부터 삭제.
// 파일 삭제 실패 시 경고 로그만 출력하고 벡터에서는 제거한다.
// ---------------------------------------------------------------------------
void PolicyVersionStore::prune() {
    while (versions_.size() > static_cast<std::size_t>(max_versions_)) {
        const auto& oldest = versions_.front();

        spdlog::info("policy_version_store: pruning old snapshot v{} ('{}')",
                     oldest.version,
                     oldest.snapshot_path.string());

        std::error_code ec;
        std::filesystem::remove(oldest.snapshot_path, ec);
        if (ec) {
            spdlog::warn("policy_version_store: failed to delete snapshot file '{}': {}",
                         oldest.snapshot_path.string(),
                         ec.message());
            // 파일 삭제 실패해도 벡터에서는 제거 (이후 접근 시 실패로 처리됨)
        }

        versions_.erase(versions_.begin());
    }
}

// ---------------------------------------------------------------------------
// compute_hash (static private)
//
// OpenSSL EVP SHA-256 으로 file_path 내용의 hex digest 를 계산한다.
// 파일 읽기 실패 또는 OpenSSL 오류 시 빈 문자열 반환.
//
// [구현 노트]
// EVP_MD_CTX 를 unique_ptr 로 관리하여 예외/오류 시 자원 누수 방지.
// 파일을 4KB 청크로 읽어 EVP_DigestUpdate 에 전달한다.
// ---------------------------------------------------------------------------
std::string PolicyVersionStore::compute_hash(const std::filesystem::path& file_path) {
    // EVP_MD_CTX RAII 래퍼
    struct EvpCtxDeleter {
        void operator()(EVP_MD_CTX* ctx) const noexcept {
            if (ctx != nullptr) {
                EVP_MD_CTX_free(ctx);
            }
        }
    };
    using EvpCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpCtxDeleter>;

    const EvpCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) {
        spdlog::warn("policy_version_store: EVP_MD_CTX_new failed");
        return {};
    }

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        spdlog::warn("policy_version_store: EVP_DigestInit_ex failed");
        return {};
    }

    // 파일 열기
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        spdlog::warn("policy_version_store: cannot open '{}' for hashing", file_path.string());
        return {};
    }

    // 4KB 청크로 읽어 해시 업데이트
    constexpr std::size_t chunk_size = 4096;
    std::array<char, chunk_size> buf{};
    while (file) {
        file.read(buf.data(), static_cast<std::streamsize>(chunk_size));
        const auto read_count = static_cast<std::size_t>(file.gcount());
        if (read_count == 0) {
            break;
        }
        if (EVP_DigestUpdate(ctx.get(), buf.data(), read_count) != 1) {
            spdlog::warn("policy_version_store: EVP_DigestUpdate failed");
            return {};
        }
    }

    if (file.bad()) {
        spdlog::warn("policy_version_store: I/O error reading '{}' for hashing",
                     file_path.string());
        return {};
    }

    // 최종 해시값 추출
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len{0};
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digest_len) != 1) {
        spdlog::warn("policy_version_store: EVP_DigestFinal_ex failed");
        return {};
    }

    // hex 문자열 변환
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (const unsigned char byte : std::span<const unsigned char>{digest.data(), digest_len}) {
        oss << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return oss.str();
}
