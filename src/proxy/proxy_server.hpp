#pragma once

#include "policy/policy_engine.hpp"
#include "policy/policy_loader.hpp"
#include "logger/structured_logger.hpp"
#include "stats/stats_collector.hpp"
#include "stats/uds_server.hpp"
#include "health/health_check.hpp"
#include "proxy/session.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

// ---------------------------------------------------------------------------
// ProxyConfig
//   ProxyServer 의 모든 설정 값을 담는다.
//   하드코딩 금지: 값은 반드시 외부(YAML/환경변수)에서 읽어야 한다.
//
//   listen_address        : 프록시가 바인딩할 IP 주소 (예: "0.0.0.0")
//   listen_port           : 프록시 리슨 포트
//   upstream_address      : 업스트림 MySQL 서버 IP/호스트명
//   upstream_port         : 업스트림 MySQL 서버 포트
//   max_connections       : 동시 허용 최대 세션 수
//   connection_timeout_sec: 세션 유휴 타임아웃 (초)
//   policy_path           : 정책 파일 경로 (YAML)
//   uds_socket_path       : Go 운영도구와 통신하는 Unix Domain Socket 경로
//   log_path              : 로그 출력 파일 경로
//   log_level             : 로그 레벨 문자열 ("trace","debug","info","warn","error")
//   health_check_port     : Health Check HTTP 서버 포트
// ---------------------------------------------------------------------------
struct ProxyConfig {
    std::string   listen_address{};
    std::uint16_t listen_port{0};

    std::string   upstream_address{};
    std::uint16_t upstream_port{0};

    std::uint32_t max_connections{0};
    std::uint32_t connection_timeout_sec{0};

    std::string   policy_path{};
    std::string   uds_socket_path{};
    std::string   log_path{};
    std::string   log_level{};

    std::uint16_t health_check_port{0};
};

// ---------------------------------------------------------------------------
// ProxyServer
//   TCP 리슨 → 세션 생성 → Graceful Shutdown 을 담당하는 메인 서버.
//
//   사용 예:
//     ProxyServer server(config);
//     server.run(io_ctx);   // io_ctx.run() 은 호출자가 실행
//
//   Graceful Shutdown:
//     stop() 호출 시 새 연결을 거부하고 기존 세션이 완료되면 io_context 를 중단한다.
// ---------------------------------------------------------------------------
class ProxyServer {
public:
    // -----------------------------------------------------------------------
    // 생성자
    //   config : 프록시 설정 (값으로 복사하여 소유)
    // -----------------------------------------------------------------------
    explicit ProxyServer(ProxyConfig config);

    ~ProxyServer() = default;

    // 복사/이동 금지
    ProxyServer(const ProxyServer&)            = delete;
    ProxyServer& operator=(const ProxyServer&) = delete;
    ProxyServer(ProxyServer&&)                 = delete;
    ProxyServer& operator=(ProxyServer&&)      = delete;

    // -----------------------------------------------------------------------
    // run
    //   io_ctx 위에서 Accept 루프를 시작한다.
    //   이 함수는 코루틴으로 spawn 되어야 하며, stop() 호출 전까지 계속 실행된다.
    // -----------------------------------------------------------------------
    void run(boost::asio::io_context& io_ctx);

    // -----------------------------------------------------------------------
    // stop
    //   Graceful Shutdown 을 시작한다.
    //   - 새 연결 Accept 중단
    //   - 진행 중인 세션은 완료될 때까지 대기
    //   - 모든 세션 종료 후 io_context 중단 요청
    //   SIGTERM 핸들러에서 호출하도록 설계되어 있다.
    // -----------------------------------------------------------------------
    void stop();

private:
    ProxyConfig config_;
    bool        stopping_{false};

    std::shared_ptr<PolicyEngine>    policy_engine_{};
    std::shared_ptr<StructuredLogger> logger_{};
    std::shared_ptr<StatsCollector>  stats_{};
    std::unique_ptr<UdsServer>       uds_server_{};
    std::unique_ptr<HealthCheck>     health_check_{};

    std::atomic<std::uint64_t>       next_session_id_{1};
    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_{};

    boost::asio::io_context*         io_ctx_{nullptr};

    // policy_reload: SIGHUP 핸들러에서 호출
    //   PolicyLoader::load() → 성공 시 policy_engine_->reload()
    //   실패 시 기존 정책 유지 + 경고 로그
    void policy_reload();

    // accept_loop: TCP Accept 루프 코루틴
    //   listen_ep : 리슨 엔드포인트 (값으로 받아 코루틴 프레임에 안전하게 보관)
    boost::asio::awaitable<void> accept_loop(boost::asio::ip::tcp::endpoint listen_ep);
};
