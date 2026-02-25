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

auto parse_lenenc_integer(std::span<const std::uint8_t> payload,
                          std::size_t&                  offset,
                          std::uint64_t&                value) -> bool
{
    if (offset >= payload.size()) {
        return false;
    }

    const std::uint8_t first = payload[offset++];
    if (first < 0xFB) {
        value = first;
        return true;
    }

    if (first == 0xFC) {
        if (offset + 2 > payload.size()) { return false; }
        value = static_cast<std::uint64_t>(payload[offset])
              | (static_cast<std::uint64_t>(payload[offset + 1]) << 8U);
        offset += 2;
        return true;
    }

    if (first == 0xFD) {
        if (offset + 3 > payload.size()) { return false; }
        value = static_cast<std::uint64_t>(payload[offset])
              | (static_cast<std::uint64_t>(payload[offset + 1]) << 8U)
              | (static_cast<std::uint64_t>(payload[offset + 2]) << 16U);
        offset += 3;
        return true;
    }

    if (first == 0xFE) {
        if (offset + 8 > payload.size()) { return false; }
        value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(payload[offset + i]) << (8ULL * i);
        }
        offset += 8;
        return true;
    }

    // 0xFB(NULL)는 lenenc integer가 아닌 row cell marker.
    return false;
}

auto consume_lenenc_text_cell(std::span<const std::uint8_t> payload, std::size_t& offset) -> bool
{
    if (offset >= payload.size()) {
        return false;
    }

    if (payload[offset] == 0xFB) {  // NULL
        ++offset;
        return true;
    }

    std::uint64_t len = 0;
    if (!parse_lenenc_integer(payload, offset, len)) {
        return false;
    }
    if (offset + len > payload.size()) {
        return false;
    }
    offset += static_cast<std::size_t>(len);
    return true;
}

auto is_text_row_packet(std::span<const std::uint8_t> payload, std::uint8_t column_count) -> bool
{
    std::size_t offset = 0;
    for (std::uint8_t i = 0; i < column_count; ++i) {
        if (!consume_lenenc_text_cell(payload, offset)) {
            return false;
        }
    }
    return offset == payload.size();
}

auto is_resultset_final_ok_packet(std::span<const std::uint8_t> payload) -> bool
{
    if (payload.empty() || payload[0] != 0x00) {
        return false;
    }

    std::size_t offset = 1;
    std::uint64_t ignored = 0;
    if (!parse_lenenc_integer(payload, offset, ignored)) {
        return false;
    }
    if (!parse_lenenc_integer(payload, offset, ignored)) {
        return false;
    }

    // status_flags(2) + warnings(2) 는 최소 필수 필드
    return offset + 4 <= payload.size();
}

auto is_metadata_terminator_packet(std::span<const std::uint8_t> payload) -> bool
{
    return (payload.size() >= 1 && payload[0] == 0xFE && payload.size() < 9)
        || is_resultset_final_ok_packet(payload);
}

auto relay_stmt_prepare_section(boost::asio::ip::tcp::socket& server_socket,
                                boost::asio::ip::tcp::socket& client_socket,
                                std::uint16_t                 count,
                                std::uint64_t                 session_id)
    -> boost::asio::awaitable<std::expected<void, ParseError>>
{
    for (std::uint16_t i = 0; i < count; ++i) {
        auto def_pkt_result = co_await read_one_packet(server_socket);
        if (!def_pkt_result) {
            co_return std::unexpected(def_pkt_result.error());
        }

        auto wr = co_await write_packet_raw(client_socket, *def_pkt_result);
        if (!wr) {
            co_return std::unexpected(wr.error());
        }
    }

    auto term_pkt_result = co_await read_one_packet(server_socket);
    if (!term_pkt_result) {
        co_return std::unexpected(term_pkt_result.error());
    }

    auto wr = co_await write_packet_raw(client_socket, *term_pkt_result);
    if (!wr) {
        co_return std::unexpected(wr.error());
    }

    const auto payload = term_pkt_result->payload();
    if (!is_metadata_terminator_packet(payload)) {
        spdlog::warn("[session {}] unexpected COM_STMT_PREPARE terminator: 0x{:02x} (len={})",
                     session_id,
                     payload.empty() ? 0U : static_cast<unsigned>(payload[0]),
                     payload.size());
    }

    co_return std::expected<void, ParseError>{};
}

}  // namespace

// ---------------------------------------------------------------------------
// relay_server_response
//   MySQL 서버 응답(OK / ERR / Result Set)이 완료될 때까지 읽어 클라이언트에 릴레이.
//
//   상태 머신 기반 구현:
//   1. 첫 패킷 분석:
//      - 0xFF → ERR, 즉시 종료
//      - 0x00 → OK (비-결과셋), 즉시 종료
//      - 0xFE && payload < 9 → EOF (비정상), 즉시 종료
//      - 0x01~0xFC → column_count (Result Set 진입)
//
//   2. Result Set 상태 머신:
//      - kColumnDefs: column_count개 definition 읽기
//      - kRows: row 데이터 읽기 (0x00~0xFB 시작 가능)
//      - 종료: EOF (0xFE, payload < 9) 또는 OK (CLIENT_DEPRECATE_EOF)
//
//   핵심: Result Set 내에서 row가 0x00으로 시작할 수 있으므로,
//         상태 추적을 통해 column EOF 이후의 0x00은 row data로 취급.
// ---------------------------------------------------------------------------
auto Session::relay_server_response(CommandType request_type, [[maybe_unused]] std::uint8_t request_seq_id)
    -> boost::asio::awaitable<std::expected<void, ParseError>>
{
    enum class ResponseState {
        kFirst,       // 첫 패킷 분석 중
        kColumnDefs,  // column definition 읽는 중
        kRows,        // row 데이터 읽는 중
        kDone,        // 응답 완료
    };

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

    // OK 패킷 (0x00)
    // - 일반 커맨드 응답: 즉시 완료
    // - COM_STMT_PREPARE : 첫 OK 뒤에 parameter/column metadata 패킷이 이어질 수 있음
    if (first_byte == 0x00) {
        if (request_type == CommandType::kComStmtPrepare) {
            if (first_payload.size() < 12) {
                spdlog::warn("[session {}] short COM_STMT_PREPARE OK payload: {} bytes",
                             session_id_, first_payload.size());
                co_return std::expected<void, ParseError>{};
            }

            const std::uint16_t num_columns =
                static_cast<std::uint16_t>(first_payload[5])
                | (static_cast<std::uint16_t>(first_payload[6]) << 8U);
            const std::uint16_t num_params =
                static_cast<std::uint16_t>(first_payload[7])
                | (static_cast<std::uint16_t>(first_payload[8]) << 8U);

            if (num_params > 0) {
                auto params_result = co_await relay_stmt_prepare_section(
                    server_socket_, client_socket_, num_params, session_id_);
                if (!params_result) {
                    co_return std::unexpected(params_result.error());
                }
            }

            if (num_columns > 0) {
                auto columns_result = co_await relay_stmt_prepare_section(
                    server_socket_, client_socket_, num_columns, session_id_);
                if (!columns_result) {
                    co_return std::unexpected(columns_result.error());
                }
            }
        }
        co_return std::expected<void, ParseError>{};
    }

    // EOF 패킷 (0xFE, payload.size() < 9) → 즉시 완료 (비정상)
    if (first_byte == 0xFE && first_payload.size() < 9) {
        co_return std::expected<void, ParseError>{};
    }

    // Result Set: 첫 바이트가 column count (0x01~0xFC)
    // column_count >= 1이면 반드시 column definition들, 필드 EOF, row 데이터, 행 EOF가 뒤따름
    if (first_byte < 0x01 || first_byte > 0xFC) {
        // 예상 범위 밖 → 비정상
        spdlog::warn("[session {}] unexpected first byte in response: 0x{:02x}",
                     session_id_, first_byte);
        co_return std::expected<void, ParseError>{};
    }

    const std::uint8_t column_count = first_byte;
    std::uint8_t column_defs_read = 0;
    ResponseState state = ResponseState::kColumnDefs;
    std::uint8_t prev_seq_id = first_pkt.sequence_id();

    while (state != ResponseState::kDone) {
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
            // 비어있는 payload → 비정상, 중단
            break;
        }

        const std::uint8_t byte0 = payload[0];

        // ERR 패킷은 모든 상태에서 즉시 종료
        if (byte0 == 0xFF) {
            state = ResponseState::kDone;
            continue;
        }

        // seq_id 역전 감지 (새 커맨드 응답 시작)
        if (pkt.sequence_id() < prev_seq_id && prev_seq_id != 0xFF) {
            spdlog::warn("[session {}] seq_id reversed ({} -> {}), stopping relay",
                         session_id_, prev_seq_id, pkt.sequence_id());
            state = ResponseState::kDone;
            continue;
        }
        prev_seq_id = pkt.sequence_id();

        // 상태별 처리
        switch (state) {
            case ResponseState::kColumnDefs: {
                // Column definition 읽는 중
                if (byte0 == 0xFE && payload.size() < 9) {
                    // Column EOF → row 데이터 준비
                    state = ResponseState::kRows;
                } else if (byte0 == 0xFF) {
                    // Error 패킷 (이미 처리됨)
                    state = ResponseState::kDone;
                } else {
                    // Column definition 패킷
                    ++column_defs_read;
                    if (column_defs_read > column_count + 1) {
                        // 정상 개수보다 많음 → 비정상
                        spdlog::warn("[session {}] too many column definitions: {} > {}",
                                     session_id_, column_defs_read, column_count);
                        state = ResponseState::kDone;
                    }
                }
                break;
            }

            case ResponseState::kRows: {
                // Row 데이터 읽는 중
                // row는 0x00~0xFB로 시작할 수 있음
                // MySQL 텍스트 프로토콜:
                //   - row 패킷: column_value들의 시퀀스 (lenenc-encoded)
                //   - EOF 패킷: header=0xFE, payload < 9
                //   - OK 패킷: header=0x00 (CLIENT_DEPRECATE_EOF 모드)
                //
                // 문제: row의 첫 컬럼이 빈 문자열(0x00)이고 다음 컬럼들이 있으면,
                //       [0x00, ...] 형태로 OK와 유사할 수 있음.
                //
                // 해결:
                //   1) 정상 row 패킷 형태면 row로 취급
                //   2) EOF(0xFE, size < 9) 또는 최종 OK(0x00 + OK 형식)면 종료
                if (byte0 == 0xFE && payload.size() < 9) {
                    // Row EOF 패킷 → 결과셋 완료
                    state = ResponseState::kDone;
                } else if (byte0 == 0xFF) {
                    // Error 패킷 (이미 처리됨, 중복 방지)
                    state = ResponseState::kDone;
                } else if (request_type == CommandType::kComQuery
                           && byte0 == 0x00
                           && !is_text_row_packet(payload, column_count)
                           && is_resultset_final_ok_packet(payload)) {
                    // CLIENT_DEPRECATE_EOF 모드의 최종 OK 패킷
                    state = ResponseState::kDone;
                } else {
                    // Row data (0x00~0xFB로 시작하는 column values)
                    // 계속 읽음
                }
                break;
            }

            case ResponseState::kDone:
            case ResponseState::kFirst:
                // 이미 완료되었으므로 루프 탈출
                break;
        }
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
                    co_await relay_server_response(cmd.command_type, cmd.sequence_id);
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
        // Prepared Statement 계열 (보안 fail-close)
        //   현재 정책 엔진은 COM_QUERY만 검사한다.
        //   COM_STMT_PREPARE/EXECUTE/RESET을 투명 릴레이하면 정책 우회 경로가 된다.
        //   statement 추적 기반 정책 적용 전까지는 명시적으로 거부한다.
        // ---------------------------------------------------------------
        if (cmd.command_type == CommandType::kComStmtPrepare ||
            cmd.command_type == CommandType::kComStmtExecute ||
            cmd.command_type == CommandType::kComStmtReset)
        {
            spdlog::warn("[session {}] blocking unsupported prepared-statement command: 0x{:02x}",
                         session_id_, static_cast<std::uint8_t>(cmd.command_type));

            const auto err_pkt = MysqlPacket::make_error(
                1235,  // ER_NOT_SUPPORTED_YET
                "Prepared statements are not supported by proxy policy enforcement",
                static_cast<std::uint8_t>(cmd.sequence_id + 1)
            );

            boost::system::error_code wr_ec;
            const auto err_bytes = err_pkt.serialize();
            co_await boost::asio::async_write(
                client_socket_,
                boost::asio::buffer(err_bytes),
                boost::asio::redirect_error(boost::asio::use_awaitable, wr_ec)
            );

            if (wr_ec) {
                spdlog::warn("[session {}] failed to send prepared-statement rejection: {}",
                             session_id_, wr_ec.message());
                break;
            }

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
            auto relay_result = co_await relay_server_response(cmd.command_type, cmd.sequence_id);
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
        // 양쪽 소켓 I/O를 취소해 진행 중인 async_read/relay 대기를 깨운다.
        boost::system::error_code ec;
        client_socket_.cancel(ec);
        server_socket_.cancel(ec);
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
