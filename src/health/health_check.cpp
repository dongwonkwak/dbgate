#include "health/health_check.hpp"
#include "stats/stats_collector.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <format>
#include <string>

// ---------------------------------------------------------------------------
// HealthCheck — HTTP/1.0 subset 구현
//
// GET /health 에 대해:
//   - kHealthy   -> HTTP 200 + {"status":"ok"}
//   - kUnhealthy -> HTTP 503 + {"status":"unhealthy","reason":"..."}
// 기타 경로  -> HTTP 404 + {"status":"not found"}
// 응답 후 소켓 즉시 close.
//
// StatsCollector 연동:
//   active_sessions >= max_connections 시 set_unhealthy() 호출.
//   (max_connections 는 ProxyServer 가 주입, HealthCheck 는 StatsCollector 만 참조)
// ---------------------------------------------------------------------------

namespace {

// -----------------------------------------------------------------------
// HTTP 응답 빌더 헬퍼
// -----------------------------------------------------------------------
std::string make_http_response(int status_code,
                               std::string_view status_text,
                               std::string_view body)
{
    return std::format(
        "HTTP/1.0 {} {}\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: {}\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{}",
        status_code,
        status_text,
        body.size(),
        body
    );
}

// -----------------------------------------------------------------------
// handle_connection
//   단일 HTTP 연결을 처리하는 코루틴.
//   요청 첫 줄만 읽어 경로를 판별하고 응답 후 소켓 close.
// -----------------------------------------------------------------------
auto handle_connection(boost::asio::ip::tcp::socket    socket,
                       const HealthStatus&             status,
                       const std::string&              unhealthy_reason,
                       const std::shared_ptr<StatsCollector>& stats)
    -> boost::asio::awaitable<void>
{
    // 요청 읽기 버퍼 (첫 줄만 필요: "GET /health HTTP/1.0\r\n" 수준)
    std::array<char, 512> buf{};
    boost::system::error_code ec;

    std::size_t n = co_await socket.async_read_some(
        boost::asio::buffer(buf),
        boost::asio::redirect_error(boost::asio::use_awaitable, ec)
    );

    if (ec) {
        spdlog::debug("[health_check] read error: {}", ec.message());
        co_return;
    }

    const std::string_view req{buf.data(), n};
    const bool is_get_health = (req.find("GET /health") != std::string_view::npos);

    std::string response;

    if (is_get_health) {
        // StatsCollector 에서 active_sessions 확인 (연동)
        // HealthCheck 외부(ProxyServer)에서 max_connections 체크를 통해
        // set_unhealthy()를 호출하는 구조이지만, 여기서도 snapshot으로 추가 확인.
        // (실제 임계값 비교는 ProxyServer가 담당하므로 여기서는 status_만 참조)
        (void)stats;  // snapshot 접근은 ProxyServer 측에서 처리

        if (status == HealthStatus::kHealthy) {
            const std::string body{R"({"status":"ok"})"};
            response = make_http_response(200, "OK", body);
        } else {
            // reason 이 비어있으면 기본 메시지 사용
            const std::string safe_reason =
                unhealthy_reason.empty() ? "service unavailable" : unhealthy_reason;

            // JSON 내부 따옴표 이스케이프 (간단 구현 — 제어문자 없음 가정)
            std::string escaped;
            escaped.reserve(safe_reason.size());
            for (char c : safe_reason) {
                if (c == '"') { escaped += "\\\""; }
                else if (c == '\\') { escaped += "\\\\"; }
                else { escaped += c; }
            }

            const std::string body = std::format(
                R"({{"status":"unhealthy","reason":"{}"}})",
                escaped
            );
            response = make_http_response(503, "Service Unavailable", body);
        }
    } else {
        const std::string body{R"({"status":"not found"})"};
        response = make_http_response(404, "Not Found", body);
    }

    // 응답 전송
    co_await boost::asio::async_write(
        socket,
        boost::asio::buffer(response),
        boost::asio::redirect_error(boost::asio::use_awaitable, ec)
    );

    if (ec) {
        spdlog::debug("[health_check] write error: {}", ec.message());
    }

    // 소켓 즉시 close
    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    socket.close(ec);
}

}  // namespace

// ---------------------------------------------------------------------------
// HealthCheck 구현
// ---------------------------------------------------------------------------

HealthCheck::HealthCheck(std::uint16_t                   port,
                         std::shared_ptr<StatsCollector> stats,
                         boost::asio::io_context&        io_context)
    : port_{port}
    , stats_{std::move(stats)}
    , io_context_{io_context}
    , status_{HealthStatus::kHealthy}
    , unhealthy_reason_{}
{}

auto HealthCheck::run() -> boost::asio::awaitable<void>
{
    const auto endpoint = boost::asio::ip::tcp::endpoint{
        boost::asio::ip::tcp::v4(),
        port_
    };

    boost::asio::ip::tcp::acceptor acceptor{io_context_, endpoint};
    acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

    spdlog::info("[health_check] listening on port {}", port_);

    while (true) {
        boost::system::error_code ec;
        auto socket = co_await acceptor.async_accept(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec)
        );

        if (ec) {
            if (ec == boost::asio::error::operation_aborted) {
                // acceptor가 닫힘 — 정상 종료
                spdlog::info("[health_check] acceptor closed, stopping");
                break;
            }
            spdlog::warn("[health_check] accept error: {}", ec.message());
            continue;
        }

        // 각 연결을 독립 코루틴으로 처리 (응답 후 즉시 close)
        boost::asio::co_spawn(
            io_context_,
            handle_connection(
                std::move(socket),
                status_,
                unhealthy_reason_,
                stats_
            ),
            boost::asio::detached
        );
    }
}

void HealthCheck::set_unhealthy(std::string_view reason)
{
    unhealthy_reason_ = std::string{reason};
    status_ = HealthStatus::kUnhealthy;
}

void HealthCheck::set_healthy()
{
    unhealthy_reason_.clear();
    status_ = HealthStatus::kHealthy;
}

auto HealthCheck::status() const noexcept -> HealthStatus
{
    return status_;
}
