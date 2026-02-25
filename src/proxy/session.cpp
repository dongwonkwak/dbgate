#include "proxy/session.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <format>
#include <span>
#include <vector>

// ---------------------------------------------------------------------------
// Session — 구현
//
// 흐름:
//   1. SessionContext 초기화
//   2. stats_.on_connection_open()
//   3. MySQL 서버 async_connect
//   4. HandshakeRelay::relay_handshake()
//   5. state_ = kReady → logger_.log_connection("connect")
//   6. 커맨드 루프:
//        헤더 4B + payload 읽기 → MysqlPacket::parse()
//        COM_QUIT → break
//        COM_QUERY → parse → policy → block/allow 분기
//        기타     → 서버 투명 릴레이
//   7. state_ = kClosed → stats/logger 정리 → 소켓 close
// ---------------------------------------------------------------------------

// 기본 SQL Injection 패턴 (InjectionDetector 초기화)
static const std::vector<std::string> kDefaultInjectionPatterns = {
    R"(UNION\s+SELECT)",
    R"(('\s*OR\s+['"\d]))",
    R"(SLEEP\s*\()",
    R"(BENCHMARK\s*\()",
    R"(LOAD_FILE\s*\()",
    R"(INTO\s+OUTFILE)",
    R"(INTO\s+DUMPFILE)",
    R"(;\s*(DROP|DELETE|UPDATE|INSERT|ALTER|CREATE))",
    R"(--\s*$)",
    R"(/\*.*\*/)",
};

// ---------------------------------------------------------------------------
// Session 생성자
// ---------------------------------------------------------------------------
Session::Session(std::uint64_t                          session_id,
                 boost::asio::ip::tcp::socket           client_socket,
                 boost::asio::ip::tcp::endpoint         server_endpoint,
                 std::shared_ptr<PolicyEngine>          policy,
                 std::shared_ptr<StructuredLogger>      logger,
                 std::shared_ptr<StatsCollector>        stats)
    : session_id_{session_id}
    , client_socket_{std::move(client_socket)}
    , server_socket_{client_socket_.get_executor()}
    , server_endpoint_{std::move(server_endpoint)}
    , policy_{std::move(policy)}
    , logger_{std::move(logger)}
    , stats_{std::move(stats)}
    , ctx_{}
    , state_{SessionState::kHandshaking}
    , strand_{boost::asio::make_strand(client_socket_.get_executor())}
    , sql_parser_{}
    , injection_detector_{kDefaultInjectionPatterns}
    , proc_detector_{}
    , closing_{false}
{}

// ---------------------------------------------------------------------------
// 내부 헬퍼: 패킷 1개 읽기 (4바이트 헤더 + payload)
// ---------------------------------------------------------------------------
namespace {

auto read_one_packet(boost::asio::ip::tcp::socket& sock)
    -> boost::asio::awaitable<std::expected<MysqlPacket, ParseError>>
{
    std::array<std::uint8_t, 4> header{};
    boost::system::error_code ec;

    co_await boost::asio::async_read(
        sock,
        boost::asio::buffer(header),
        boost::asio::redirect_error(boost::asio::use_awaitable, ec)
    );

    if (ec) {
        co_return std::unexpected(ParseError{
            ParseErrorCode::kMalformedPacket,
            "failed to read packet header",
            ec.message()
        });
    }

    const std::uint32_t payload_len =
        static_cast<std::uint32_t>(header[0])
        | (static_cast<std::uint32_t>(header[1]) << 8U)
        | (static_cast<std::uint32_t>(header[2]) << 16U);

    std::vector<std::uint8_t> buf(4 + payload_len);
    buf[0] = header[0];
    buf[1] = header[1];
    buf[2] = header[2];
    buf[3] = header[3];

    if (payload_len > 0) {
        co_await boost::asio::async_read(
            sock,
            boost::asio::buffer(buf.data() + 4, payload_len),
            boost::asio::redirect_error(boost::asio::use_awaitable, ec)
        );
        if (ec) {
            co_return std::unexpected(ParseError{
                ParseErrorCode::kMalformedPacket,
                "failed to read packet payload",
                ec.message()
            });
        }
    }

    co_return MysqlPacket::parse(std::span<const std::uint8_t>{buf});
}

auto write_packet_raw(boost::asio::ip::tcp::socket& sock, const MysqlPacket& pkt)
    -> boost::asio::awaitable<std::expected<void, ParseError>>
{
    const auto bytes = pkt.serialize();
    boost::system::error_code ec;

    co_await boost::asio::async_write(
        sock,
        boost::asio::buffer(bytes),
        boost::asio::redirect_error(boost::asio::use_awaitable, ec)
    );

    if (ec) {
        co_return std::unexpected(ParseError{
            ParseErrorCode::kInternalError,
            "failed to write packet",
            ec.message()
        });
    }

    co_return std::expected<void, ParseError>{};
}

}  // namespace

// ---------------------------------------------------------------------------
// relay_server_response
//   MySQL 서버 응답(OK / ERR / Result Set)이 완료될 때까지 읽어 클라이언트에 릴레이.
//
//   Result Set 완료 감지:
//     - 첫 패킷 0xFF → ERR, 즉시 반환
//     - 첫 패킷 0x00 / seq_id 역전 → OK, 즉시 반환
//     - 0xFE + payload < 9 → EOF 패킷 → Result Set 또는 필드 목록 종료
//     - seq_id가 이전보다 작아지는 경우(역전) → 새 응답 시작이므로 완료
//
//   간단한 구현 전략:
//     seq_id 단조 증가를 기준으로 역전 감지.
//     EOF 패킷(0xFE, payload < 9) 2회 감지 → Result Set 완료 (필드 EOF + 행 EOF).
//     단, CLIENT_DEPRECATE_EOF 사용 시 OK 패킷으로 대체되므로 OK 감지도 처리.
// ---------------------------------------------------------------------------
auto Session::relay_server_response(std::uint8_t request_seq_id)
    -> boost::asio::awaitable<std::expected<void, ParseError>>
{
    // 첫 패킷으로 응답 유형 판별
    auto first_pkt_result = co_await read_one_packet(server_socket_);
    if (!first_pkt_result) {
        co_return std::unexpected(first_pkt_result.error());
    }

    const MysqlPacket& first_pkt = *first_pkt_result;
    const auto first_payload = first_pkt.payload();

    // 첫 패킷을 클라이언트에 전달
    auto wr = co_await write_packet_raw(client_socket_, first_pkt);
    if (!wr) {
        co_return std::unexpected(wr.error());
    }

    if (first_payload.empty()) {
        // 비어있는 payload — 비정상이지만 계속 진행
        co_return std::expected<void, ParseError>{};
    }

    const std::uint8_t first_byte = first_payload[0];

    // ERR 패킷 (0xFF) → 즉시 완료
    if (first_byte == 0xFF) {
        co_return std::expected<void, ParseError>{};
    }

    // OK 패킷 (0x00, payload < 9 즉 EOF 아님) → 즉시 완료
    // CLIENT_DEPRECATE_EOF 환경에서 OK로 Result Set 종료 가능
    if (first_byte == 0x00) {
        co_return std::expected<void, ParseError>{};
    }

    // EOF 패킷 (0xFE, payload.size() < 9) → 즉시 완료
    if (first_byte == 0xFE && first_payload.size() < 9) {
        co_return std::expected<void, ParseError>{};
    }

    // Result Set: 첫 바이트가 column count (>0)
    // 열 정의 패킷들 → 필드 EOF → 행 데이터 패킷들 → 행 EOF(또는 OK)
    // seq_id 역전 방지를 위해 이전 seq_id 추적
    std::uint8_t prev_seq_id = first_pkt.sequence_id();
    int eof_count = 0;  // EOF 패킷 카운터 (2개면 Result Set 완료)

    while (true) {
        auto pkt_result = co_await read_one_packet(server_socket_);
        if (!pkt_result) {
            co_return std::unexpected(pkt_result.error());
        }

        const MysqlPacket& pkt = *pkt_result;
        const auto payload = pkt.payload();

        // 클라이언트에 전달
        auto w = co_await write_packet_raw(client_socket_, pkt);
        if (!w) {
            co_return std::unexpected(w.error());
        }

        if (payload.empty()) {
            break;
        }

        const std::uint8_t byte0 = payload[0];

        // ERR 패킷 → 즉시 종료
        if (byte0 == 0xFF) {
            break;
        }

        // OK 패킷 (CLIENT_DEPRECATE_EOF 모드) → 즉시 종료
        // 주의: 결과셋 행의 첫 컬럼이 빈 문자열이면 0x00으로 시작할 수 있음
        // OK 패킷은 최소 5바이트: header(0x00) + affected_rows + last_insert_id + status_flags(2)
        if (byte0 == 0x00 && payload.size() >= 5) {
            break;
        }

        // EOF 패킷
        if (byte0 == 0xFE && payload.size() < 9) {
            ++eof_count;
            if (eof_count >= 2) {
                // 필드 EOF + 행 EOF → Result Set 완료
                break;
            }
        }

        // seq_id 역전 감지 (새 커맨드 응답 시작 — 비정상, 안전하게 중단)
        if (pkt.sequence_id() < prev_seq_id && prev_seq_id != 0xFF) {
            spdlog::warn("[session {}] seq_id reversed ({} -> {}), stopping relay",
                         session_id_, prev_seq_id, pkt.sequence_id());
            break;
        }

        prev_seq_id = pkt.sequence_id();
    }

    co_return std::expected<void, ParseError>{};
}

// ---------------------------------------------------------------------------
// Session::run
// ---------------------------------------------------------------------------
auto Session::run() -> boost::asio::awaitable<void>
{
    // -----------------------------------------------------------------------
    // 1. SessionContext 초기화
    // -----------------------------------------------------------------------
    ctx_.session_id   = session_id_;
    ctx_.connected_at = std::chrono::system_clock::now();

    // 클라이언트 IP/포트 추출
    boost::system::error_code peer_ec;
    const auto remote_ep = client_socket_.remote_endpoint(peer_ec);
    if (!peer_ec) {
        ctx_.client_ip   = remote_ep.address().to_string();
        ctx_.client_port = remote_ep.port();
    }

    // -----------------------------------------------------------------------
    // 2. stats: 연결 열기
    // -----------------------------------------------------------------------
    stats_->on_connection_open();

    // 세션 종료 시 정리 보장을 위한 RAII guard
    // (co_await 도중 예외 없이도 반드시 on_connection_close 호출)
    struct StatsGuard {
        StatsCollector* stats;
        ~StatsGuard() { stats->on_connection_close(); }
    } stats_guard{stats_.get()};

    // -----------------------------------------------------------------------
    // 3. MySQL 서버 async_connect
    // -----------------------------------------------------------------------
    boost::system::error_code connect_ec;
    co_await boost::asio::async_connect(
        server_socket_,
        std::vector{server_endpoint_},
        boost::asio::redirect_error(boost::asio::use_awaitable, connect_ec)
    );

    if (connect_ec) {
        spdlog::error("[session {}] upstream connect failed: {}",
                      session_id_, connect_ec.message());

        // 클라이언트에 MySQL ERR Packet 반환
        const auto err_pkt = MysqlPacket::make_error(
            2003,   // ER_CONN_HOST_ERROR
            std::format("Can't connect to MySQL server ({})", connect_ec.message()),
            0
        );
        boost::system::error_code wr_ec;
        const auto err_bytes = err_pkt.serialize();
        co_await boost::asio::async_write(
            client_socket_,
            boost::asio::buffer(err_bytes),
            boost::asio::redirect_error(boost::asio::use_awaitable, wr_ec)
        );

        state_ = SessionState::kClosed;
        boost::system::error_code close_ec;
        client_socket_.shutdown(
            boost::asio::ip::tcp::socket::shutdown_both, close_ec);
        client_socket_.close(close_ec);
        co_return;
    }

    // -----------------------------------------------------------------------
    // 4. HandshakeRelay::relay_handshake()
    // -----------------------------------------------------------------------
    auto hs_result = co_await HandshakeRelay::relay_handshake(
        client_socket_, server_socket_, ctx_
    );

    if (!hs_result) {
        spdlog::error("[session {}] handshake failed: {}",
                      session_id_, hs_result.error().message);
        state_ = SessionState::kClosed;
        boost::system::error_code close_ec;
        client_socket_.shutdown(
            boost::asio::ip::tcp::socket::shutdown_both, close_ec);
        client_socket_.close(close_ec);
        server_socket_.shutdown(
            boost::asio::ip::tcp::socket::shutdown_both, close_ec);
        server_socket_.close(close_ec);
        co_return;
    }

    // -----------------------------------------------------------------------
    // 5. 핸드셰이크 완료 → kReady
    // -----------------------------------------------------------------------
    state_ = SessionState::kReady;

    logger_->log_connection(ConnectionLog{
        .session_id  = session_id_,
        .event       = "connect",
        .client_ip   = ctx_.client_ip,
        .client_port = ctx_.client_port,
        .db_user     = ctx_.db_user,
        .timestamp   = std::chrono::system_clock::now(),
    });

    spdlog::info("[session {}] handshake done, user={}, db={}",
                 session_id_, ctx_.db_user, ctx_.db_name);

    // -----------------------------------------------------------------------
    // 6. 커맨드 루프
    // -----------------------------------------------------------------------
    while (true) {
        // closing_ 플래그 확인 — Graceful Shutdown 중이면 루프 탈출
        if (closing_.load(std::memory_order_acquire)) {
            break;
        }

        // 클라이언트로부터 패킷 읽기
        auto pkt_result = co_await read_one_packet(client_socket_);

        if (!pkt_result) {
            const auto& err = pkt_result.error();
            // EOF / 연결 종료는 정상 종료로 처리
            if (err.context.find("eof") != std::string::npos ||
                err.context.find("End of file") != std::string::npos ||
                err.message.find("header") != std::string::npos)
            {
                spdlog::debug("[session {}] client disconnected", session_id_);
            } else {
                spdlog::warn("[session {}] read error: {} (context: {})",
                             session_id_, err.message, err.context);
            }
            break;
        }

        const MysqlPacket& pkt = *pkt_result;

        // 커맨드 추출
        auto cmd_result = extract_command(pkt);
        if (!cmd_result) {
            spdlog::warn("[session {}] malformed command packet: {}",
                         session_id_, cmd_result.error().message);
            // malformed → fail-close: 세션 종료
            break;
        }

        const CommandPacket& cmd = *cmd_result;

        // ---------------------------------------------------------------
        // COM_QUIT
        // ---------------------------------------------------------------
        if (cmd.command_type == CommandType::kComQuit) {
            spdlog::debug("[session {}] COM_QUIT received", session_id_);
            // 서버에도 전달 (서버 측 정상 종료)
            boost::system::error_code fwd_ec;
            const auto quit_bytes = pkt.serialize();
            co_await boost::asio::async_write(
                server_socket_,
                boost::asio::buffer(quit_bytes),
                boost::asio::redirect_error(boost::asio::use_awaitable, fwd_ec)
            );
            break;
        }

        // ---------------------------------------------------------------
        // COM_QUERY
        // ---------------------------------------------------------------
        if (cmd.command_type == CommandType::kComQuery) {
            state_ = SessionState::kProcessingQuery;

            const auto query_start = std::chrono::steady_clock::now();

            // SQL 파싱
            auto parse_result = sql_parser_.parse(cmd.query);

            PolicyResult policy_result;

            if (!parse_result) {
                // 파싱 실패 → evaluate_error → fail-close (kBlock)
                spdlog::warn("[session {}] SQL parse error: {} sql={}",
                             session_id_,
                             parse_result.error().message,
                             cmd.query.size() > 200
                                 ? cmd.query.substr(0, 200) + "..."
                                 : cmd.query);
                policy_result = policy_->evaluate_error(parse_result.error(), ctx_);
            } else {
                // 파싱 성공 → InjectionDetector / ProcedureDetector (참고용)
                const ParsedQuery& parsed = *parse_result;

                [[maybe_unused]] const auto inj_result  = injection_detector_.check(cmd.query);
                [[maybe_unused]] const auto proc_result = proc_detector_.detect(parsed);

                // 정책 평가
                policy_result = policy_->evaluate(parsed, ctx_);
            }

            const auto query_end = std::chrono::steady_clock::now();
            const auto duration  = std::chrono::duration_cast<std::chrono::microseconds>(
                query_end - query_start
            );

            // ---------------------------------------------------------------
            // kBlock: 클라이언트에 ERR 응답 + 로그 + stats
            // ---------------------------------------------------------------
            if (policy_result.action == PolicyAction::kBlock) {
                const auto err_pkt = MysqlPacket::make_error(
                    1045,  // ER_ACCESS_DENIED_ERROR
                    "Access denied by policy",
                    static_cast<std::uint8_t>(cmd.sequence_id + 1)
                );

                boost::system::error_code wr_ec;
                const auto err_bytes = err_pkt.serialize();
                co_await boost::asio::async_write(
                    client_socket_,
                    boost::asio::buffer(err_bytes),
                    boost::asio::redirect_error(boost::asio::use_awaitable, wr_ec)
                );

                logger_->log_block(BlockLog{
                    .session_id   = session_id_,
                    .db_user      = ctx_.db_user,
                    .client_ip    = ctx_.client_ip,
                    .raw_sql      = cmd.query,
                    .matched_rule = policy_result.matched_rule,
                    .reason       = policy_result.reason,
                    .timestamp    = std::chrono::system_clock::now(),
                });

                stats_->on_query(true);  // blocked = true

                state_ = SessionState::kReady;
                continue;
            }

            // ---------------------------------------------------------------
            // kAllow / kLog: 서버에 릴레이 → 응답 릴레이 → 로그 + stats
            // ---------------------------------------------------------------
            {
                // 쿼리 패킷 서버로 전달
                auto fwd = co_await write_packet_raw(server_socket_, pkt);
                if (!fwd) {
                    spdlog::error("[session {}] failed to forward query to server: {}",
                                  session_id_, fwd.error().message);
                    break;
                }

                // 서버 응답 릴레이
                auto relay_result =
                    co_await relay_server_response(cmd.sequence_id);
                if (!relay_result) {
                    spdlog::warn("[session {}] relay_server_response failed: {}",
                                 session_id_, relay_result.error().message);
                    break;
                }

                // 파싱 결과 재추출 (로그용)
                std::uint8_t command_raw = 0;
                std::vector<std::string> tables;
                if (parse_result) {
                    command_raw = static_cast<std::uint8_t>(parse_result->command);
                    tables      = parse_result->tables;
                }

                logger_->log_query(QueryLog{
                    .session_id  = session_id_,
                    .db_user     = ctx_.db_user,
                    .client_ip   = ctx_.client_ip,
                    .raw_sql     = cmd.query,
                    .command_raw = command_raw,
                    .tables      = std::move(tables),
                    .action_raw  = static_cast<std::uint8_t>(policy_result.action),
                    .timestamp   = std::chrono::system_clock::now(),
                    .duration    = duration,
                });

                stats_->on_query(false);  // blocked = false
            }

            state_ = SessionState::kReady;
            continue;
        }

        // ---------------------------------------------------------------
        // 기타 커맨드: 서버로 투명 릴레이 + 응답 클라이언트에 릴레이
        // ---------------------------------------------------------------
        {
            auto fwd = co_await write_packet_raw(server_socket_, pkt);
            if (!fwd) {
                spdlog::warn("[session {}] failed to forward command to server: {}",
                             session_id_, fwd.error().message);
                break;
            }

            // COM_STMT_PREPARE 등 다중 패킷 응답도 정확히 릴레이
            auto relay_result = co_await relay_server_response(cmd.sequence_id);
            if (!relay_result) {
                spdlog::warn("[session {}] failed to relay server response: {}",
                             session_id_, relay_result.error().message);
                break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // 7. 세션 정리
    // -----------------------------------------------------------------------
    state_ = SessionState::kClosed;

    logger_->log_connection(ConnectionLog{
        .session_id  = session_id_,
        .event       = "disconnect",
        .client_ip   = ctx_.client_ip,
        .client_port = ctx_.client_port,
        .db_user     = ctx_.db_user,
        .timestamp   = std::chrono::system_clock::now(),
    });

    spdlog::info("[session {}] closed", session_id_);

    // 소켓 닫기
    boost::system::error_code close_ec;
    client_socket_.shutdown(
        boost::asio::ip::tcp::socket::shutdown_both, close_ec);
    client_socket_.close(close_ec);
    server_socket_.shutdown(
        boost::asio::ip::tcp::socket::shutdown_both, close_ec);
    server_socket_.close(close_ec);

    // stats_guard 소멸 시 on_connection_close() 호출됨
}

// ---------------------------------------------------------------------------
// Session::close
//   Graceful Shutdown: closing_ 플래그 설정.
//   진행 중인 쿼리가 완료된 후 커맨드 루프가 탈출한다.
// ---------------------------------------------------------------------------
void Session::close()
{
    bool expected = false;
    if (closing_.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel)) {
        spdlog::debug("[session {}] close() called", session_id_);
        // 소켓 close를 통해 진행 중인 async_read를 중단시킨다.
        boost::system::error_code ec;
        client_socket_.cancel(ec);
    }
}

// ---------------------------------------------------------------------------
// Session::state / context
// ---------------------------------------------------------------------------
auto Session::state() const noexcept -> SessionState
{
    return state_;
}

auto Session::context() const noexcept -> const SessionContext&
{
    return ctx_;
}
