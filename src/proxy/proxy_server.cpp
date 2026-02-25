#include "proxy/proxy_server.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <spdlog/spdlog.h>

#include <format>

// ---------------------------------------------------------------------------
// ProxyServer — 구현
//
// run() 흐름:
//   1. io_ctx_ 저장
//   2. PolicyLoader::load(config_.policy_path)
//   3. logger_, stats_, policy_engine_ 생성
//   4. uds_server_ + co_spawn(run)
//   5. health_check_ + co_spawn(run)
//   6. SIGTERM/SIGINT 핸들러 + SIGHUP 핸들러
//   7. accept 루프: 세션 생성 + co_spawn(session->run())
//      콜백에서 sessions_.erase()
//
// stop() 흐름:
//   1. stopping_ = true
//   2. acceptor.close()
//   3. 활성 세션 각각 session->close()
//   4. 세션 0개이면 io_ctx_->stop()
// ---------------------------------------------------------------------------

ProxyServer::ProxyServer(ProxyConfig config)
    : config_{std::move(config)}
{}

// ---------------------------------------------------------------------------
// LogLevel 문자열 → LogLevel 변환
// ---------------------------------------------------------------------------
namespace {

LogLevel parse_log_level(const std::string& level_str)
{
    if (level_str == "debug") { return LogLevel::kDebug; }
    if (level_str == "warn")  { return LogLevel::kWarn;  }
    if (level_str == "error") { return LogLevel::kError; }
    return LogLevel::kInfo;
}

}  // namespace

// ---------------------------------------------------------------------------
// policy_reload
// ---------------------------------------------------------------------------
void ProxyServer::policy_reload()
{
    spdlog::info("[proxy] SIGHUP received — reloading policy: {}",
                 config_.policy_path);

    auto result = PolicyLoader::load(config_.policy_path);
    if (!result) {
        spdlog::warn("[proxy] policy reload failed (keeping current policy): {}",
                     result.error());
        return;
    }

    auto new_config = std::make_shared<PolicyConfig>(std::move(*result));
    policy_engine_->reload(std::move(new_config));
    spdlog::info("[proxy] policy reloaded successfully");
}

// ---------------------------------------------------------------------------
// ProxyServer::run
// ---------------------------------------------------------------------------
void ProxyServer::run(boost::asio::io_context& io_ctx)
{
    io_ctx_ = &io_ctx;

    // -----------------------------------------------------------------------
    // 2. PolicyLoader::load
    // -----------------------------------------------------------------------
    std::shared_ptr<PolicyConfig> policy_config;

    auto load_result = PolicyLoader::load(config_.policy_path);
    if (!load_result) {
        spdlog::warn("[proxy] initial policy load failed (fail-close — all queries blocked): {}",
                     load_result.error());
        // nullptr → PolicyEngine은 모든 evaluate()에서 kBlock 반환 (fail-close)
        policy_config = nullptr;
    } else {
        policy_config = std::make_shared<PolicyConfig>(std::move(*load_result));
        spdlog::info("[proxy] policy loaded from: {}", config_.policy_path);
    }

    // -----------------------------------------------------------------------
    // 3. logger, stats, policy_engine 생성
    // -----------------------------------------------------------------------
    const auto log_level = parse_log_level(config_.log_level);
    logger_        = std::make_shared<StructuredLogger>(log_level, config_.log_path);
    stats_         = std::make_shared<StatsCollector>();
    policy_engine_ = std::make_shared<PolicyEngine>(std::move(policy_config));

    // -----------------------------------------------------------------------
    // 4. UdsServer 생성 + co_spawn
    // -----------------------------------------------------------------------
    uds_server_ = std::make_unique<UdsServer>(
        config_.uds_socket_path,
        stats_,
        io_ctx
    );

    boost::asio::co_spawn(
        io_ctx,
        uds_server_->run(),
        [](std::exception_ptr eptr) {
            if (eptr) {
                try { std::rethrow_exception(eptr); }
                catch (const std::exception& e) {
                    spdlog::error("[proxy] uds_server error: {}", e.what());
                }
            }
        }
    );

    // -----------------------------------------------------------------------
    // 5. HealthCheck 생성 + co_spawn
    // -----------------------------------------------------------------------
    health_check_ = std::make_unique<HealthCheck>(
        config_.health_check_port,
        stats_,
        io_ctx
    );

    boost::asio::co_spawn(
        io_ctx,
        health_check_->run(),
        [](std::exception_ptr eptr) {
            if (eptr) {
                try { std::rethrow_exception(eptr); }
                catch (const std::exception& e) {
                    spdlog::error("[proxy] health_check error: {}", e.what());
                }
            }
        }
    );

    // -----------------------------------------------------------------------
    // 6. 시그널 핸들러
    //    SIGTERM / SIGINT → stop()
    //    SIGHUP           → policy_reload()
    // -----------------------------------------------------------------------
    auto signals_stop = std::make_shared<boost::asio::signal_set>(
        io_ctx, SIGTERM, SIGINT
    );
    signals_stop->async_wait(
        [this](const boost::system::error_code& ec, int /*signum*/) {
            if (!ec) {
                spdlog::info("[proxy] shutdown signal received");
                stop();
            }
        }
    );

    auto signals_hup = std::make_shared<boost::asio::signal_set>(io_ctx, SIGHUP);
    // SIGHUP 핸들러: 수신 후 재등록하여 반복 감지
    std::function<void()> setup_hup;
    setup_hup = [this, &io_ctx, signals_hup, &setup_hup]() {
        signals_hup->async_wait(
            [this, &setup_hup](const boost::system::error_code& ec, int /*signum*/) {
                if (!ec) {
                    policy_reload();
                    setup_hup();  // 다시 대기
                }
            }
        );
    };
    setup_hup();

    // -----------------------------------------------------------------------
    // 7. Accept 루프 (co_spawn)
    // -----------------------------------------------------------------------
    const auto listen_addr = boost::asio::ip::make_address(config_.listen_address);
    const auto listen_ep   = boost::asio::ip::tcp::endpoint{listen_addr, config_.listen_port};

    boost::asio::co_spawn(
        io_ctx,
        // accept 루프 코루틴
        [this, listen_ep, &io_ctx]() -> boost::asio::awaitable<void> {
            boost::asio::ip::tcp::acceptor acceptor{io_ctx, listen_ep};
            acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

            spdlog::info("[proxy] listening on {}:{}",
                         config_.listen_address, config_.listen_port);

            while (!stopping_) {
                boost::system::error_code ec;
                auto client_sock = co_await acceptor.async_accept(
                    boost::asio::redirect_error(boost::asio::use_awaitable, ec)
                );

                if (ec) {
                    if (ec == boost::asio::error::operation_aborted) {
                        spdlog::info("[proxy] acceptor closed");
                        break;
                    }
                    if (!stopping_) {
                        spdlog::warn("[proxy] accept error: {}", ec.message());
                    }
                    continue;
                }

                if (stopping_) {
                    // 이미 종료 중 — 소켓 즉시 닫기
                    boost::system::error_code close_ec;
                    client_sock.close(close_ec);
                    continue;
                }

                // max_connections 초과 시 unhealthy 전환 + 연결 거부
                if (config_.max_connections > 0) {
                    const auto snap = stats_->snapshot();
                    if (snap.active_sessions >= config_.max_connections) {
                        spdlog::warn(
                            "[proxy] max_connections ({}) reached, rejecting new connection",
                            config_.max_connections
                        );
                        health_check_->set_unhealthy(
                            std::format("max_connections ({}) reached",
                                        config_.max_connections)
                        );
                        boost::system::error_code close_ec;
                        client_sock.close(close_ec);
                        continue;
                    }

                    // 세션 수가 회복되면 healthy 전환
                    if (health_check_->status() == HealthStatus::kUnhealthy &&
                        snap.active_sessions < config_.max_connections)
                    {
                        health_check_->set_healthy();
                    }
                }

                // 세션 생성
                const std::uint64_t sid = next_session_id_.fetch_add(
                    1, std::memory_order_relaxed
                );

                const auto server_ep = boost::asio::ip::tcp::endpoint{
                    boost::asio::ip::make_address(config_.upstream_address),
                    config_.upstream_port
                };

                auto session = std::make_shared<Session>(
                    sid,
                    std::move(client_sock),
                    server_ep,
                    policy_engine_,
                    logger_,
                    stats_
                );

                sessions_.emplace(sid, session);

                spdlog::debug("[proxy] new session {}", sid);

                // 세션 코루틴 spawn — 완료 시 sessions_ 에서 제거
                boost::asio::co_spawn(
                    io_ctx,
                    session->run(),
                    [this, sid](std::exception_ptr eptr) {
                        if (eptr) {
                            try { std::rethrow_exception(eptr); }
                            catch (const std::exception& e) {
                                spdlog::error("[proxy] session {} exception: {}",
                                              sid, e.what());
                            }
                        }

                        sessions_.erase(sid);
                        spdlog::debug("[proxy] session {} removed (active: {})",
                                      sid, sessions_.size());

                        // 모든 세션 종료 + stopping 중 → io_context 중단
                        if (stopping_ && sessions_.empty()) {
                            spdlog::info("[proxy] all sessions closed, stopping io_context");
                            io_ctx_->stop();
                        }
                    }
                );
            }
        }(),
        [](std::exception_ptr eptr) {
            if (eptr) {
                try { std::rethrow_exception(eptr); }
                catch (const std::exception& e) {
                    spdlog::error("[proxy] accept loop exception: {}", e.what());
                }
            }
        }
    );
}

// ---------------------------------------------------------------------------
// ProxyServer::stop
//   Graceful Shutdown.
//   1. stopping_ = true
//   2. 활성 세션 각각 close()
//   3. 세션이 없으면 즉시 io_ctx_ stop
// ---------------------------------------------------------------------------
void ProxyServer::stop()
{
    if (stopping_) {
        return;
    }

    stopping_ = true;

    spdlog::info("[proxy] stopping — active sessions: {}", sessions_.size());

    // HealthCheck를 unhealthy로 전환 (로드밸런서 라우팅 제외)
    if (health_check_) {
        health_check_->set_unhealthy("proxy shutting down");
    }

    // UdsServer 정지
    if (uds_server_) {
        uds_server_->stop();
    }

    // 활성 세션 전체 close()
    for (auto& [sid, session] : sessions_) {
        spdlog::debug("[proxy] closing session {}", sid);
        session->close();
    }

    // 세션이 없으면 즉시 종료
    if (sessions_.empty() && io_ctx_ != nullptr) {
        spdlog::info("[proxy] no active sessions, stopping io_context immediately");
        io_ctx_->stop();
    }
}
