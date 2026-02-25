// ---------------------------------------------------------------------------
// test_proxy_server.cpp
//
// ProxyServer 및 Session의 기본 동작 단위 테스트.
//
// 네트워크 I/O가 필요한 end-to-end 테스트는 별도 통합 테스트에서 수행한다.
// 여기서는 생성/초기화/상태 전이 경로 등 비I/O 항목을 검증한다.
// ---------------------------------------------------------------------------

#include "proxy/proxy_server.hpp"
#include "proxy/session.hpp"
#include "health/health_check.hpp"
#include "stats/stats_collector.hpp"
#include "policy/policy_engine.hpp"
#include "logger/structured_logger.hpp"
#include "common/types.hpp"

#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <memory>
#include <string>
#include <filesystem>
#include <fstream>

// ---------------------------------------------------------------------------
// 테스트 픽스처 헬퍼
// ---------------------------------------------------------------------------
namespace {

std::shared_ptr<PolicyEngine> make_policy_engine()
{
    // nullptr config → fail-close (모든 쿼리 차단)
    return std::make_shared<PolicyEngine>(nullptr);
}

std::shared_ptr<StructuredLogger> make_logger()
{
    // 임시 파일에 로그 출력
    const auto tmp_path = std::filesystem::temp_directory_path()
                          / "test_proxy_logger.log";
    return std::make_shared<StructuredLogger>(LogLevel::kDebug, tmp_path);
}

std::shared_ptr<StatsCollector> make_stats()
{
    return std::make_shared<StatsCollector>();
}

}  // namespace

// ---------------------------------------------------------------------------
// ProxyConfig 기본값 테스트
// ---------------------------------------------------------------------------
TEST(ProxyConfigTest, DefaultValues)
{
    ProxyConfig cfg;
    EXPECT_EQ(cfg.listen_port, 0);
    EXPECT_EQ(cfg.upstream_port, 0);
    EXPECT_EQ(cfg.max_connections, 0U);
    EXPECT_EQ(cfg.connection_timeout_sec, 0U);
    EXPECT_EQ(cfg.health_check_port, 0);
    EXPECT_TRUE(cfg.listen_address.empty());
    EXPECT_TRUE(cfg.upstream_address.empty());
    EXPECT_TRUE(cfg.policy_path.empty());
    EXPECT_TRUE(cfg.uds_socket_path.empty());
    EXPECT_TRUE(cfg.log_path.empty());
    EXPECT_TRUE(cfg.log_level.empty());
}

// ---------------------------------------------------------------------------
// ProxyServer 생성자 테스트
// ---------------------------------------------------------------------------
TEST(ProxyServerTest, ConstructionWithConfig)
{
    ProxyConfig cfg;
    cfg.listen_address   = "127.0.0.1";
    cfg.listen_port      = 13306;
    cfg.upstream_address = "127.0.0.1";
    cfg.upstream_port    = 3306;
    cfg.max_connections  = 100;
    cfg.health_check_port = 18080;
    cfg.log_level        = "info";
    cfg.log_path         = "/tmp/test_proxy.log";
    cfg.uds_socket_path  = "/tmp/test_proxy.sock";
    cfg.policy_path      = "/tmp/nonexistent_policy.yaml";

    // 생성만 검증 (run() 호출 없음)
    EXPECT_NO_THROW({
        ProxyServer server{cfg};
    });
}

// ---------------------------------------------------------------------------
// HealthCheck 상태 전이 테스트
// ---------------------------------------------------------------------------
TEST(HealthCheckTest, InitialStateIsHealthy)
{
    boost::asio::io_context ioc;
    auto stats = make_stats();
    HealthCheck hc{18081, stats, ioc};

    EXPECT_EQ(hc.status(), HealthStatus::kHealthy);
}

TEST(HealthCheckTest, SetUnhealthyTransition)
{
    boost::asio::io_context ioc;
    auto stats = make_stats();
    HealthCheck hc{18082, stats, ioc};

    hc.set_unhealthy("overloaded");
    EXPECT_EQ(hc.status(), HealthStatus::kUnhealthy);
}

TEST(HealthCheckTest, SetHealthyRecovery)
{
    boost::asio::io_context ioc;
    auto stats = make_stats();
    HealthCheck hc{18083, stats, ioc};

    hc.set_unhealthy("test");
    EXPECT_EQ(hc.status(), HealthStatus::kUnhealthy);

    hc.set_healthy();
    EXPECT_EQ(hc.status(), HealthStatus::kHealthy);
}

TEST(HealthCheckTest, SetUnhealthyEmptyReason)
{
    boost::asio::io_context ioc;
    auto stats = make_stats();
    HealthCheck hc{18084, stats, ioc};

    hc.set_unhealthy("");
    EXPECT_EQ(hc.status(), HealthStatus::kUnhealthy);
}

TEST(HealthCheckTest, MultipleSetUnhealthyCalls)
{
    boost::asio::io_context ioc;
    auto stats = make_stats();
    HealthCheck hc{18085, stats, ioc};

    hc.set_unhealthy("reason1");
    EXPECT_EQ(hc.status(), HealthStatus::kUnhealthy);

    hc.set_unhealthy("reason2");
    EXPECT_EQ(hc.status(), HealthStatus::kUnhealthy);
}

// ---------------------------------------------------------------------------
// Session 생성자 + 상태 초기값 테스트
// ---------------------------------------------------------------------------
TEST(SessionTest, InitialState)
{
    boost::asio::io_context ioc;

    // 로컬 TCP 소켓 생성 (실제 연결 없음)
    boost::asio::ip::tcp::socket sock{ioc};

    const auto server_ep = boost::asio::ip::tcp::endpoint{
        boost::asio::ip::address_v4::loopback(),
        3306
    };

    auto session = std::make_shared<Session>(
        42ULL,
        std::move(sock),
        server_ep,
        make_policy_engine(),
        make_logger(),
        make_stats()
    );

    EXPECT_EQ(session->state(), SessionState::kHandshaking);
    EXPECT_EQ(session->context().session_id, 0ULL);  // 아직 run() 전
}

TEST(SessionTest, ContextAfterConstruction)
{
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket sock{ioc};

    const auto server_ep = boost::asio::ip::tcp::endpoint{
        boost::asio::ip::address_v4::loopback(),
        3306
    };

    auto session = std::make_shared<Session>(
        99ULL,
        std::move(sock),
        server_ep,
        make_policy_engine(),
        make_logger(),
        make_stats()
    );

    // run() 전에는 ctx_ 기본값
    const auto& ctx = session->context();
    EXPECT_TRUE(ctx.client_ip.empty());
    EXPECT_EQ(ctx.client_port, 0);
    EXPECT_FALSE(ctx.handshake_done);
    EXPECT_TRUE(ctx.db_user.empty());
    EXPECT_TRUE(ctx.db_name.empty());
}

TEST(SessionTest, CloseIdempotent)
{
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket sock{ioc};

    const auto server_ep = boost::asio::ip::tcp::endpoint{
        boost::asio::ip::address_v4::loopback(),
        3306
    };

    auto session = std::make_shared<Session>(
        1ULL,
        std::move(sock),
        server_ep,
        make_policy_engine(),
        make_logger(),
        make_stats()
    );

    // close()는 idempotent — 여러 번 호출해도 크래시 없어야 함
    EXPECT_NO_THROW({
        session->close();
        session->close();
        session->close();
    });
}

// ---------------------------------------------------------------------------
// SessionState 열거형 값 검증
// ---------------------------------------------------------------------------
TEST(SessionStateTest, EnumValues)
{
    EXPECT_EQ(static_cast<std::uint8_t>(SessionState::kHandshaking),     0U);
    EXPECT_EQ(static_cast<std::uint8_t>(SessionState::kReady),           1U);
    EXPECT_EQ(static_cast<std::uint8_t>(SessionState::kProcessingQuery), 2U);
    EXPECT_EQ(static_cast<std::uint8_t>(SessionState::kClosing),         3U);
    EXPECT_EQ(static_cast<std::uint8_t>(SessionState::kClosed),          4U);
}

// ---------------------------------------------------------------------------
// StatsCollector 연동 테스트 (HealthCheck 과부하 감지)
// ---------------------------------------------------------------------------
TEST(StatsHealthIntegrationTest, OverloadDetectionLogic)
{
    boost::asio::io_context ioc;
    auto stats = make_stats();
    HealthCheck hc{18086, stats, ioc};

    // 초기 상태: healthy
    EXPECT_EQ(hc.status(), HealthStatus::kHealthy);

    // 5개 연결 열기 시뮬레이션
    for (int i = 0; i < 5; ++i) {
        stats->on_connection_open();
    }

    const auto snap = stats->snapshot();
    EXPECT_EQ(snap.active_sessions, 5ULL);

    // max_connections = 5 초과 시 unhealthy 전환 (ProxyServer 로직 시뮬레이션)
    constexpr std::uint32_t max_conn = 5;
    if (snap.active_sessions >= max_conn) {
        hc.set_unhealthy("max_connections reached");
    }
    EXPECT_EQ(hc.status(), HealthStatus::kUnhealthy);

    // 연결 닫기 → 회복 시뮬레이션
    stats->on_connection_close();
    const auto snap2 = stats->snapshot();
    if (snap2.active_sessions < max_conn) {
        hc.set_healthy();
    }
    EXPECT_EQ(hc.status(), HealthStatus::kHealthy);
}
