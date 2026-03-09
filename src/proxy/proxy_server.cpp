#include "proxy/proxy_server.hpp"

#include <spdlog/spdlog.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <format>

// ---------------------------------------------------------------------------
// ProxyServer — 구현
//
// run() 흐름:
//   1. io_ctx_ 저장
//   2. SSL context 초기화 (설정에 따라)
//   3. PolicyLoader::load(config_.policy_path)
//   4. logger_, stats_, policy_engine_ 생성
//   5. uds_server_ + co_spawn(run)
//   6. health_check_ + co_spawn(run)
//   7. SIGTERM/SIGINT 핸들러 + SIGHUP 핸들러
//   8. accept 루프: 세션 생성 + co_spawn(session->run())
//      콜백에서 sessions_.erase()
// ---------------------------------------------------------------------------

ProxyServer::ProxyServer(ProxyConfig config) : config_{std::move(config)} {}

// ---------------------------------------------------------------------------
// LogLevel 문자열 → LogLevel 변환
// ---------------------------------------------------------------------------
namespace {

LogLevel parse_log_level(const std::string& level_str) {
    if (level_str == "debug") {
        return LogLevel::kDebug;
    }
    if (level_str == "warn") {
        return LogLevel::kWarn;
    }
    if (level_str == "error") {
        return LogLevel::kError;
    }
    return LogLevel::kInfo;
}

}  // namespace

// ---------------------------------------------------------------------------
// init_ssl
//   SSL/TLS context를 초기화한다.
//   - Frontend SSL: cert/key 로드, TLS 1.2 최소, 약한 cipher 비활성화
//   - Backend SSL: CA 로드, 서버 인증서 검증, SNI 설정
//   - 오류 시 false 반환 (fail-close: 서버 기동 실패)
//   - SSL 비활성화 시 즉시 true 반환 (no-op)
// ---------------------------------------------------------------------------
[[nodiscard]] bool ProxyServer::init_ssl() {
    // ── Frontend SSL ────────────────────────────────────────────────────────
    if (config_.frontend_ssl_enabled) {
        spdlog::info("[proxy] frontend SSL enabled, cert={}, key={}",
                     config_.frontend_ssl_cert_path,
                     config_.frontend_ssl_key_path);

        // ssl::context 생성 (TLS 1.2 이상)
        frontend_ssl_ctx_.emplace(boost::asio::ssl::context::tls_server);
        auto& ctx = *frontend_ssl_ctx_;

        // TLS 1.0, 1.1, SSLv2/v3 비활성화
        ctx.set_options(
            boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 | boost::asio::ssl::context::no_tlsv1 |
            boost::asio::ssl::context::no_tlsv1_1 | boost::asio::ssl::context::single_dh_use);

        // 약한 cipher 비활성화
        SSL_CTX_set_cipher_list(
            ctx.native_handle(),
            "ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM:DHE+CHACHA20:!aNULL:!eNULL:"
            "!EXPORT:!DES:!RC4:!3DES:!MD5:!PSK");  // NOLINT(bugprone-unused-return-value,cert-err33-c)

        // 인증서 로드
        boost::system::error_code ec;
        // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
        ctx.use_certificate_chain_file(config_.frontend_ssl_cert_path, ec);
        if (ec) {
            spdlog::error("[proxy] frontend SSL: failed to load certificate {}: {}",
                          config_.frontend_ssl_cert_path,
                          ec.message());
            return false;
        }

        // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
        ctx.use_private_key_file(config_.frontend_ssl_key_path, boost::asio::ssl::context::pem, ec);
        if (ec) {
            spdlog::error("[proxy] frontend SSL: failed to load private key {}: {}",
                          config_.frontend_ssl_key_path,
                          ec.message());
            return false;
        }

        spdlog::info("[proxy] frontend SSL context initialized");
    }

    // ── Backend SSL ─────────────────────────────────────────────────────────
    if (config_.backend_ssl_enabled) {
        spdlog::info("[proxy] backend SSL enabled, ca={}, verify_peer={}",
                     config_.backend_ssl_ca_path,
                     config_.backend_ssl_verify);

        backend_ssl_ctx_.emplace(boost::asio::ssl::context::tls_client);
        auto& ctx = *backend_ssl_ctx_;

        ctx.set_options(boost::asio::ssl::context::default_workarounds |
                        boost::asio::ssl::context::no_sslv2 | boost::asio::ssl::context::no_sslv3 |
                        boost::asio::ssl::context::no_tlsv1 |
                        boost::asio::ssl::context::no_tlsv1_1);

        // 서버 인증서 검증 모드 설정
        if (config_.backend_ssl_verify) {
            ctx.set_verify_mode(boost::asio::ssl::verify_peer);
        } else {
            spdlog::warn("[proxy] backend SSL: peer verification disabled (insecure)");
            ctx.set_verify_mode(boost::asio::ssl::verify_none);
        }

        // CA 인증서 로드 (경로 지정 시)
        if (!config_.backend_ssl_ca_path.empty()) {
            boost::system::error_code ec;
            // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
            ctx.load_verify_file(config_.backend_ssl_ca_path, ec);
            if (ec) {
                spdlog::error("[proxy] backend SSL: failed to load CA file {}: {}",
                              config_.backend_ssl_ca_path,
                              ec.message());
                return false;
            }
        } else {
            // CA 경로 미지정: 시스템 기본 CA 사용
            boost::system::error_code ec;
            ctx.set_default_verify_paths(ec);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
            if (ec) {
                spdlog::warn("[proxy] backend SSL: set_default_verify_paths failed: {}",
                             ec.message());
            }
        }

        // SNI 호스트명 설정
        if (!config_.upstream_ssl_sni.empty()) {
            // SNI는 각 ssl::stream 인스턴스에서 설정 (Session에서 처리)
            spdlog::debug("[proxy] backend SSL: SNI hostname={}", config_.upstream_ssl_sni);
        }

        spdlog::info("[proxy] backend SSL context initialized");
    }

    return true;
}

// ---------------------------------------------------------------------------
// policy_reload
//   SIGHUP 핸들러에서 호출된다.
//   DON-50: PolicyLoader::load → save_snapshot → policy_engine_->reload(버전 포함)
// ---------------------------------------------------------------------------
void ProxyServer::policy_reload() {
    spdlog::info("[proxy] SIGHUP received — reloading policy: {}", config_.policy_path);

    auto result = PolicyLoader::load(config_.policy_path);
    if (!result) {
        spdlog::warn("[proxy] policy reload failed (keeping current policy): {}", result.error());
        return;
    }

    auto new_config = std::make_shared<PolicyConfig>(std::move(*result));

    // DON-50: 스냅샷 저장 후 버전 포함 reload
    std::uint64_t new_version = 0;
    if (version_store_) {
        const std::filesystem::path policy_path{config_.policy_path};
        auto save_result = version_store_->save_snapshot(*new_config, policy_path);
        if (!save_result) {
            spdlog::warn("[proxy] SIGHUP reload: snapshot save failed (non-fatal): {}",
                         save_result.error());
            new_version = version_store_->current_version();
        } else {
            new_version = save_result->version;
            spdlog::info("[proxy] SIGHUP reload: snapshot saved v{}", new_version);
        }
        policy_engine_->reload(new_config, new_version);
    } else {
        policy_engine_->reload(new_config);
    }

    spdlog::info("[proxy] policy reloaded successfully (v{})", new_version);
}

// ---------------------------------------------------------------------------
// ProxyServer::run
// ---------------------------------------------------------------------------
void ProxyServer::run(boost::asio::io_context& io_ctx) {
    io_ctx_ = &io_ctx;

    // -----------------------------------------------------------------------
    // 2. SSL context 초기화 (fail-close: 실패 시 기동 중단)
    // -----------------------------------------------------------------------
    if (!init_ssl()) {
        spdlog::error("[proxy] SSL initialization failed — aborting startup");
        return;
    }

    // -----------------------------------------------------------------------
    // 3. PolicyLoader::load
    // -----------------------------------------------------------------------
    std::shared_ptr<PolicyConfig> policy_config;

    auto load_result = PolicyLoader::load(config_.policy_path);
    if (!load_result) {
        spdlog::warn("[proxy] initial policy load failed (fail-close — all queries blocked): {}",
                     load_result.error());
        policy_config = nullptr;
    } else {
        policy_config = std::make_shared<PolicyConfig>(std::move(*load_result));
        spdlog::info("[proxy] policy loaded from: {}", config_.policy_path);
    }

    // -----------------------------------------------------------------------
    // 4. logger, stats, policy_engine 생성
    // -----------------------------------------------------------------------
    const auto log_level = parse_log_level(config_.log_level);
    logger_ = std::make_shared<StructuredLogger>(log_level, config_.log_path);

    // 글로벌 spdlog 레벨을 config.log_level에 맞춰 설정한다.
    // 이를 설정하지 않으면 spdlog::info() 등의 글로벌 호출이
    // LOG_LEVEL 환경변수와 무관하게 항상 출력된다.
    switch (log_level) {
        case LogLevel::kDebug:
            spdlog::set_level(spdlog::level::debug);
            break;
        case LogLevel::kWarn:
            spdlog::set_level(spdlog::level::warn);
            break;
        case LogLevel::kError:
            spdlog::set_level(spdlog::level::err);
            break;
        default:
            spdlog::set_level(spdlog::level::info);
            break;
    }

    stats_ = std::make_shared<StatsCollector>();
    policy_engine_ = std::make_shared<PolicyEngine>(policy_config);

    // -----------------------------------------------------------------------
    // 4b. PolicyVersionStore 생성 + 초기 스냅샷 저장 (DON-50)
    // -----------------------------------------------------------------------
    {
        const std::filesystem::path policy_path{config_.policy_path};
        const auto config_dir = policy_path.parent_path().empty() ? std::filesystem::current_path()
                                                                  : policy_path.parent_path();
        version_store_ = std::make_shared<PolicyVersionStore>(config_dir);

        if (policy_config) {
            // 초기 로드 성공 시 스냅샷 저장 (버전 관리 시작)
            auto save_result = version_store_->save_snapshot(*policy_config, policy_path);
            if (!save_result) {
                spdlog::warn("[proxy] initial snapshot save failed (non-fatal): {}",
                             save_result.error());
            } else {
                // 초기 버전으로 policy_engine_ 버전 설정
                policy_engine_->reload(policy_config, save_result->version);
                spdlog::info("[proxy] initial policy snapshot saved: v{}", save_result->version);
            }
        }
    }

    // -----------------------------------------------------------------------
    // 5. UdsServer 생성 + co_spawn (DON-50: version_store_ 주입)
    // -----------------------------------------------------------------------
    auto sql_parser = std::make_shared<SqlParser>();
    uds_server_ = std::make_unique<UdsServer>(config_.uds_socket_path,
                                              stats_,
                                              policy_engine_,
                                              sql_parser,
                                              version_store_,
                                              std::filesystem::path{config_.policy_path},
                                              io_ctx);

    // DON-53: UDS 보안 설정 주입
    uds_server_->set_client_timeout(config_.uds_client_timeout_sec);
    uds_server_->set_max_connections(config_.uds_max_connections);
    if (config_.uds_allowed_uid >= 0) {
        uds_server_->set_allowed_uid(static_cast<uid_t>(config_.uds_allowed_uid));
    }

    boost::asio::co_spawn(
        io_ctx,
        uds_server_->run(),
        [](std::exception_ptr eptr) {  // NOLINT(performance-unnecessary-value-param)
            if (eptr) {
                try {
                    std::rethrow_exception(eptr);
                } catch (const std::exception& e) {
                    spdlog::error("[proxy] uds_server error: {}", e.what());
                }
            }
        });

    // -----------------------------------------------------------------------
    // 6. HealthCheck 생성 + co_spawn
    // -----------------------------------------------------------------------
    health_check_ = std::make_unique<HealthCheck>(config_.health_check_port, stats_, io_ctx);

    boost::asio::co_spawn(
        io_ctx,
        health_check_->run(),
        [](std::exception_ptr eptr) {  // NOLINT(performance-unnecessary-value-param)
            if (eptr) {
                try {
                    std::rethrow_exception(eptr);
                } catch (const std::exception& e) {
                    spdlog::error("[proxy] health_check error: {}", e.what());
                }
            }
        });

    // -----------------------------------------------------------------------
    // 7. 시그널 핸들러
    // -----------------------------------------------------------------------
    auto signals_stop = std::make_shared<boost::asio::signal_set>(io_ctx, SIGTERM, SIGINT);
    signals_stop->async_wait(
        [this, signals_stop](const boost::system::error_code& ec, int /*signum*/) {
            if (!ec) {
                spdlog::info("[proxy] shutdown signal received");
                stop();
            }
        });

    auto signals_hup = std::make_shared<boost::asio::signal_set>(io_ctx, SIGHUP);
    auto setup_hup = std::make_shared<std::function<void()>>();
    *setup_hup = [this, signals_hup, setup_hup]() {
        signals_hup->async_wait(
            [this, setup_hup](const boost::system::error_code& ec, int /*signum*/) {
                if (!ec) {
                    policy_reload();
                    (*setup_hup)();
                }
            });
    };
    (*setup_hup)();

    // -----------------------------------------------------------------------
    // 8. Accept 루프 (co_spawn)
    // -----------------------------------------------------------------------
    const auto listen_addr = boost::asio::ip::make_address(config_.listen_address);
    const auto listen_ep = boost::asio::ip::tcp::endpoint{listen_addr, config_.listen_port};

    boost::asio::co_spawn(
        io_ctx,
        accept_loop(listen_ep),
        [](std::exception_ptr eptr) {  // NOLINT(performance-unnecessary-value-param)
            if (eptr) {
                try {
                    std::rethrow_exception(eptr);
                } catch (const std::exception& e) {
                    spdlog::error("[proxy] accept loop exception: {}", e.what());
                }
            }
        });
}

// ---------------------------------------------------------------------------
// ProxyServer::accept_loop
//   TCP Accept 루프 코루틴.
//   Frontend SSL이 활성화된 경우 accept 후 TLS 핸드셰이크를 수행한다.
// ---------------------------------------------------------------------------
boost::asio::awaitable<void> ProxyServer::accept_loop(boost::asio::ip::tcp::endpoint listen_ep) {
    boost::asio::ip::tcp::acceptor acceptor{*io_ctx_, listen_ep};
    acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

    spdlog::info("[proxy] listening on {}:{} (SSL={})",
                 config_.listen_address,
                 config_.listen_port,
                 config_.frontend_ssl_enabled ? "frontend" : "none");

    while (!stopping_) {
        boost::system::error_code ec;
        auto client_sock = co_await acceptor.async_accept(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));

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
            boost::system::error_code close_ec;
            client_sock.close(close_ec);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
            continue;
        }

        // TCP_NODELAY: Nagle 알고리즘 비활성화 (클라이언트 소켓)
        {
            boost::system::error_code nodelay_ec;
            // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
            client_sock.set_option(boost::asio::ip::tcp::no_delay(true), nodelay_ec);
            if (nodelay_ec) {
                spdlog::warn("[proxy] failed to set TCP_NODELAY on client socket: {}",
                             nodelay_ec.message());
            }
        }

        // max_connections 초과 시 unhealthy 전환 + 연결 거부
        if (config_.max_connections > 0) {
            const auto snap = stats_->snapshot();
            if (snap.active_sessions >= config_.max_connections) {
                spdlog::warn("[proxy] max_connections ({}) reached, rejecting new connection",
                             config_.max_connections);
                health_check_->set_unhealthy(
                    std::format("max_connections ({}) reached", config_.max_connections));
                boost::system::error_code close_ec;
                client_sock.close(close_ec);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
                continue;
            }

            if (health_check_->status() == HealthStatus::kUnhealthy &&
                snap.active_sessions < config_.max_connections) {
                health_check_->set_healthy();
            }
        }

        // 세션 ID 할당
        const std::uint64_t sid = next_session_id_.fetch_add(1, std::memory_order_relaxed);

        // upstream 호스트명 해석
        boost::asio::ip::tcp::resolver resolver{*io_ctx_};
        boost::system::error_code resolve_ec;
        auto resolve_results = co_await resolver.async_resolve(
            config_.upstream_address,
            std::to_string(config_.upstream_port),
            boost::asio::redirect_error(boost::asio::use_awaitable, resolve_ec));

        if (resolve_ec || resolve_results.empty()) {
            spdlog::error("[proxy] failed to resolve upstream {}: {}",
                          config_.upstream_address,
                          resolve_ec.message());
            boost::system::error_code close_ec;
            client_sock.close(close_ec);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
            continue;
        }

        const auto server_ep = *resolve_results.begin();

        // ──────────────────────────────────────────────────────────────────
        // Frontend SSL 처리
        //   SSL이 활성화된 경우: ssl::stream으로 래핑 (핸드셰이크는 Session::run에서 수행)
        //   SSL이 비활성화된 경우: tcp::socket → AsyncStream
        // ──────────────────────────────────────────────────────────────────
        AsyncStream client_stream{AsyncStream::tcp_socket{client_sock.get_executor()}};

        if (frontend_ssl_ctx_.has_value()) {
            AsyncStream::ssl_socket ssl_client_sock{std::move(client_sock), *frontend_ssl_ctx_};
            client_stream = AsyncStream{std::move(ssl_client_sock)};
        } else {
            client_stream = AsyncStream{std::move(client_sock)};
        }

        // Backend SSL context 포인터 (nullptr이면 평문)
        // NOLINTNEXTLINE(misc-const-correctness)
        boost::asio::ssl::context* backend_ssl_ctx_ptr =
            backend_ssl_ctx_.has_value() ? &(*backend_ssl_ctx_) : nullptr;

        const auto backend_tls_server_name =
            config_.upstream_ssl_sni.empty() ? config_.upstream_address : config_.upstream_ssl_sni;

        // 세션 생성
        auto session = std::make_shared<Session>(sid,
                                                 std::move(client_stream),
                                                 server_ep,
                                                 backend_ssl_ctx_ptr,
                                                 config_.backend_ssl_verify,
                                                 backend_tls_server_name,
                                                 policy_engine_,
                                                 logger_,
                                                 stats_);

        sessions_.emplace(sid, session);

        spdlog::debug("[proxy] new session {}", sid);

        // 세션 코루틴 spawn
        boost::asio::co_spawn(
            session->executor(),
            session->run(),
            [this, sid](std::exception_ptr eptr) {  // NOLINT(performance-unnecessary-value-param)
                if (eptr) {
                    try {
                        std::rethrow_exception(eptr);
                    } catch (const std::exception& e) {
                        spdlog::error("[proxy] session {} exception: {}", sid, e.what());
                    }
                }

                sessions_.erase(sid);
                spdlog::debug("[proxy] session {} removed (active: {})", sid, sessions_.size());

                if (stopping_ && sessions_.empty()) {
                    spdlog::info("[proxy] all sessions closed, stopping io_context");
                    io_ctx_->stop();
                }
            });
    }
}

// ---------------------------------------------------------------------------
// ProxyServer::stop
// ---------------------------------------------------------------------------
void ProxyServer::stop() {
    if (stopping_) {
        return;
    }

    stopping_ = true;

    spdlog::info("[proxy] stopping — active sessions: {}", sessions_.size());

    if (health_check_) {
        health_check_->set_unhealthy("proxy shutting down");
    }

    if (uds_server_) {
        uds_server_->stop();
    }

    for (auto& [sid, session] : sessions_) {
        spdlog::debug("[proxy] closing session {}", sid);
        session->close();
    }

    if (sessions_.empty() && io_ctx_ != nullptr) {
        spdlog::info("[proxy] no active sessions, stopping io_context immediately");
        io_ctx_->stop();
    }
}
