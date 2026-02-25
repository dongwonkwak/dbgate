#include "proxy/proxy_server.hpp"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

// ---------------------------------------------------------------------------
// Helper: 환경변수 읽기 (없으면 기본값 반환)
// ---------------------------------------------------------------------------
namespace {

std::string env_str(const char* name, std::string default_val) {
    const char* val = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    if (val != nullptr && val[0] != '\0') {
        return val;
    }
    return default_val;
}

uint16_t env_u16(const char* name, uint16_t default_val) {
    const char* val = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    if (val == nullptr || val[0] == '\0') {
        return default_val;
    }
    try {
        const int parsed = std::stoi(val);
        if (parsed < 1 || parsed > 65535) {
            spdlog::warn("env {}: value {} out of range, using default {}", name, parsed, default_val);
            return default_val;
        }
        return static_cast<uint16_t>(parsed);
    } catch (...) {
        spdlog::warn("env {}: invalid value '{}', using default {}", name, val, default_val);
        return default_val;
    }
}

uint32_t env_u32(const char* name, uint32_t default_val) {
    const char* val = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    if (val == nullptr || val[0] == '\0') {
        return default_val;
    }
    try {
        const long parsed = std::stol(val);
        if (parsed < 0) {
            spdlog::warn("env {}: negative value {}, using default {}", name, parsed, default_val);
            return default_val;
        }
        return static_cast<uint32_t>(parsed);
    } catch (...) {
        spdlog::warn("env {}: invalid value '{}', using default {}", name, val, default_val);
        return default_val;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int /*argc*/, char* /*argv*/[]) {

    // ── 설정 로드 (환경변수 우선, 기본값 fallback) ───────────────────────
    ProxyConfig config;
    config.upstream_address     = env_str("MYSQL_HOST",           "127.0.0.1");
    config.upstream_port        = env_u16("MYSQL_PORT",           3306);
    config.listen_address       = env_str("PROXY_LISTEN_ADDR",    "0.0.0.0");
    config.listen_port          = env_u16("PROXY_LISTEN_PORT",    13306);
    config.policy_path          = env_str("POLICY_PATH",          "config/policy.yaml");
    config.uds_socket_path      = env_str("UDS_SOCKET_PATH",      "/tmp/dbgate.sock");
    config.log_path             = env_str("LOG_PATH",             "/tmp/dbgate.log");
    config.log_level            = env_str("LOG_LEVEL",            "info");
    config.health_check_port    = env_u16("HEALTH_CHECK_PORT",    8080);
    config.max_connections      = env_u32("MAX_CONNECTIONS",      1000);
    config.connection_timeout_sec = env_u32("CONNECTION_TIMEOUT_SEC", 30);

    // ── 로깅 초기화 ─────────────────────────────────────────────────────
    spdlog::info("Starting dbgate proxy server");
    spdlog::info("Listen: {}:{}", config.listen_address, config.listen_port);
    spdlog::info("Upstream: {}:{}", config.upstream_address, config.upstream_port);
    spdlog::info("Policy: {}", config.policy_path);
    spdlog::info("UDS socket: {}", config.uds_socket_path);
    spdlog::info("Log level: {}", config.log_level);

    // ── ProxyServer 생성 및 실행 ────────────────────────────────────────
    boost::asio::io_context ioc;
    ProxyServer server{config};
    server.run(ioc);
    ioc.run();

    // ── 종료 처리 ───────────────────────────────────────────────────────
    spdlog::info("Proxy server stopped");

    return EXIT_SUCCESS;
}
