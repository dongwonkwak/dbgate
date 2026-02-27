// ---------------------------------------------------------------------------
// uds_server.cpp
//
// UdsServer 구현 — Unix Domain Socket 서버.
// Go CLI/대시보드에 StatsSnapshot을 노출한다.
//
// [프로토콜]
//   요청/응답 모두 4byte LE 길이 프리픽스 + JSON 바디.
//
// [지원 커맨드]
//   "stats"         — StatsSnapshot JSON 반환
//   "sessions"      — 501 placeholder (Phase 3)
//   "policy_reload" — 501 placeholder (Phase 3)
//   기타            — error 응답
//
// [격리 원칙]
//   UDS I/O 실패는 데이터패스로 전파하지 않는다.
//   stats 접근은 read-only (StatsCollector::snapshot()) 만 수행한다.
// ---------------------------------------------------------------------------

#include "stats/uds_server.hpp"

#include <spdlog/spdlog.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// json_escape
//   JSON 문자열 값 이스케이프 (따옴표 없이 내용만 반환).
// ---------------------------------------------------------------------------
std::string json_escape(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 8);
    for (char c : sv) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8]{};
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// serialize_snapshot
//   StatsSnapshot → JSON 문자열 (nlohmann/json 없이 수동 직렬화).
//   captured_at는 Unix epoch 밀리초로 직렬화한다.
// ---------------------------------------------------------------------------
std::string serialize_snapshot(const StatsSnapshot& s) {
    const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        s.captured_at.time_since_epoch()).count();

    return fmt::format(
        R"({{"total_connections":{},"active_sessions":{},"total_queries":{},"blocked_queries":{},"qps":{:.4f},"block_rate":{:.4f},"captured_at_ms":{}}})",
        s.total_connections,
        s.active_sessions,
        s.total_queries,
        s.blocked_queries,
        s.qps,
        s.block_rate,
        epoch_ms
    );
}

// ---------------------------------------------------------------------------
// make_ok_response
//   {"ok":true,"payload":<data>}
// ---------------------------------------------------------------------------
std::string make_ok_response(std::string_view data) {
    return fmt::format(R"({{"ok":true,"payload":{}}})", data);
}

// ---------------------------------------------------------------------------
// make_error_response
//   {"ok":false,"error":"<msg>"}
// ---------------------------------------------------------------------------
std::string make_error_response(std::string_view msg) {
    return fmt::format(R"({{"ok":false,"error":"{}"}})", json_escape(msg));
}

// ---------------------------------------------------------------------------
// make_not_implemented_response
//   {"ok":false,"error":"not implemented","code":501}
// ---------------------------------------------------------------------------
std::string make_not_implemented_response(std::string_view cmd) {
    return fmt::format(
        R"({{"ok":false,"error":"not implemented","code":501,"command":"{}"}})",
        json_escape(cmd)
    );
}

// ---------------------------------------------------------------------------
// encode_le4
//   uint32_t → 4바이트 LE 배열
// ---------------------------------------------------------------------------
std::array<uint8_t, 4> encode_le4(uint32_t val) {
    return {
        static_cast<uint8_t>(val),
        static_cast<uint8_t>(val >> 8),
        static_cast<uint8_t>(val >> 16),
        static_cast<uint8_t>(val >> 24),
    };
}

// ---------------------------------------------------------------------------
// decode_le4
//   4바이트 LE 배열 → uint32_t
// ---------------------------------------------------------------------------
uint32_t decode_le4(const std::array<uint8_t, 4>& buf) {
    return static_cast<uint32_t>(buf[0])
         | (static_cast<uint32_t>(buf[1]) << 8)
         | (static_cast<uint32_t>(buf[2]) << 16)
         | (static_cast<uint32_t>(buf[3]) << 24);
}

// ---------------------------------------------------------------------------
// parse_command
//   JSON 요청 문자열에서 "command" 필드를 단순 파싱한다.
//   "command":"<value>" 패턴만 지원 (nlohmann/json 의존 없음).
//   파싱 실패 시 빈 문자열 반환.
// ---------------------------------------------------------------------------
std::string parse_command(std::string_view json) {
    constexpr std::string_view key = R"("command":)";
    const auto pos = json.find(key);
    if (pos == std::string_view::npos) {
        return {};
    }
    auto start = pos + key.size();
    // 공백 건너뜀
    while (start < json.size() && json[start] == ' ') { ++start; }
    if (start >= json.size() || json[start] != '"') {
        return {};
    }
    ++start; // 여는 따옴표 건너뜀
    std::string cmd;
    while (start < json.size()) {
        const char c = json[start++];
        if (c == '"') { break; }
        if (c == '\\' && start < json.size()) {
            cmd += json[start++]; // 단순 escape 처리
        } else {
            cmd += c;
        }
    }
    return cmd;
}

// 단일 클라이언트에서 수신할 최대 메시지 크기 (4MiB)
constexpr uint32_t kMaxRequestSize = 4u * 1024u * 1024u;

} // namespace

// ---------------------------------------------------------------------------
// UdsServer 생성자/소멸자
// ---------------------------------------------------------------------------
UdsServer::UdsServer(const std::filesystem::path&    socket_path,
                     std::shared_ptr<StatsCollector> stats,
                     asio::io_context&               ioc)
    : socket_path_{socket_path}
    , stats_{std::move(stats)}
    , ioc_{ioc}
    , acceptor_{ioc}
{}

UdsServer::~UdsServer() {
    stop();
}

// ---------------------------------------------------------------------------
// stop
//   acceptor를 닫아 run()의 accept 루프를 종료한다.
// ---------------------------------------------------------------------------
void UdsServer::stop() {
    if (stop_requested_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    auto close_acceptor = [this]() {
        boost::system::error_code cancel_ec;
        acceptor_.cancel(cancel_ec);
        if (cancel_ec && cancel_ec != asio::error::bad_descriptor) {
            spdlog::warn("[uds_server] stop: acceptor cancel error: {}", cancel_ec.message());
        }

        boost::system::error_code close_ec;
        acceptor_.close(close_ec);
        if (close_ec && close_ec != asio::error::bad_descriptor) {
            spdlog::warn("[uds_server] stop: acceptor close error: {}", close_ec.message());
        }
    };

    // acceptor 소유 스레드(io_context)에서 정리해 TSan 경합을 방지한다.
    if (ioc_.stopped()) {
        close_acceptor();
        return;
    }
    asio::post(ioc_, std::move(close_acceptor));
}

// ---------------------------------------------------------------------------
// run
//   기존 소켓 파일 제거 → bind/listen → accept 루프.
//   accept 오류 시 로그 후 루프 종료.
// ---------------------------------------------------------------------------
asio::awaitable<void> UdsServer::run() {
    using stream_protocol = asio::local::stream_protocol;

    if (stop_requested_.load(std::memory_order_acquire)) {
        co_return;
    }

    // 기존 소켓 파일 제거 (bind 실패 방지)
    std::error_code fs_ec;
    std::filesystem::remove(socket_path_, fs_ec);
    if (fs_ec && fs_ec != std::make_error_code(std::errc::no_such_file_or_directory)) {
        spdlog::error("[uds_server] failed to remove old socket {}: {}",
                      socket_path_.string(), fs_ec.message());
        co_return;
    }

    // acceptor 열기
    boost::system::error_code ec;
    acceptor_.open(stream_protocol(), ec);
    if (ec) {
        spdlog::error("[uds_server] open error: {}", ec.message());
        co_return;
    }

    acceptor_.bind(stream_protocol::endpoint{socket_path_.string()}, ec);
    if (ec) {
        spdlog::error("[uds_server] bind error on {}: {}", socket_path_.string(), ec.message());
        co_return;
    }

    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        spdlog::error("[uds_server] listen error: {}", ec.message());
        co_return;
    }

    spdlog::info("[uds_server] listening on {}", socket_path_.string());

    // accept 루프
    for (;;) {
        if (stop_requested_.load(std::memory_order_acquire)) {
            co_return;
        }

        stream_protocol::socket client_socket{ioc_};
        auto [accept_ec] = co_await acceptor_.async_accept(
            client_socket, asio::as_tuple(asio::use_awaitable));

        if (accept_ec) {
            if (accept_ec == asio::error::operation_aborted ||
                accept_ec == boost::system::errc::bad_file_descriptor) {
                // stop() 으로 acceptor가 닫힌 경우 — 정상 종료
                spdlog::info("[uds_server] accept loop stopped");
            } else {
                spdlog::error("[uds_server] accept error: {}", accept_ec.message());
            }
            co_return;
        }

        // 클라이언트 처리 코루틴을 독립적으로 spawn (실패가 서버에 영향 없음)
        asio::co_spawn(
            ioc_,
            handle_client(std::move(client_socket)),
            asio::detached
        );
    }
}

// ---------------------------------------------------------------------------
// handle_client
//   단일 클라이언트 연결 처리:
//     1. 4바이트 LE 헤더로 요청 크기 읽기
//     2. JSON 바디 읽기
//     3. 커맨드 디스패치
//     4. 4바이트 LE 헤더 + JSON 바디 응답 송신
//
//   오류 발생 시 로그 후 co_return (데이터패스 비전파).
// ---------------------------------------------------------------------------
asio::awaitable<void> UdsServer::handle_client(
    asio::local::stream_protocol::socket socket)
{
    // ── 요청 헤더 읽기 ──────────────────────────────────────────────────
    std::array<uint8_t, 4> req_hdr{};
    auto [hdr_ec, hdr_n] = co_await asio::async_read(
        socket, asio::buffer(req_hdr),
        asio::as_tuple(asio::use_awaitable));

    if (hdr_ec) {
        if (hdr_ec != asio::error::eof) {
            spdlog::warn("[uds_server] handle_client: read header error: {}", hdr_ec.message());
        }
        co_return;
    }
    if (hdr_n != 4) {
        spdlog::warn("[uds_server] handle_client: short header ({} bytes)", hdr_n);
        co_return;
    }

    const uint32_t body_len = decode_le4(req_hdr);
    if (body_len == 0 || body_len > kMaxRequestSize) {
        spdlog::warn("[uds_server] handle_client: invalid body length {}", body_len);
        co_return;
    }

    // ── 요청 바디 읽기 ──────────────────────────────────────────────────
    std::vector<char> body_buf(body_len);
    auto [body_ec, body_n] = co_await asio::async_read(
        socket, asio::buffer(body_buf),
        asio::as_tuple(asio::use_awaitable));

    if (body_ec) {
        spdlog::warn("[uds_server] handle_client: read body error: {}", body_ec.message());
        co_return;
    }
    if (body_n != body_len) {
        spdlog::warn("[uds_server] handle_client: short body ({}/{} bytes)", body_n, body_len);
        co_return;
    }

    const std::string_view request_json{body_buf.data(), body_n};

    // ── 커맨드 디스패치 ─────────────────────────────────────────────────
    const std::string cmd = parse_command(request_json);
    std::string response_body;

    if (cmd == "stats") {
        // stats — StatsSnapshot 직렬화 후 반환
        const StatsSnapshot snap = stats_->snapshot();
        response_body = make_ok_response(serialize_snapshot(snap));
    } else if (cmd == "sessions") {
        // Phase 3 구현 예정
        response_body = make_not_implemented_response("sessions");
    } else if (cmd == "policy_reload") {
        // Phase 3 구현 예정
        response_body = make_not_implemented_response("policy_reload");
    } else if (cmd.empty()) {
        spdlog::warn("[uds_server] handle_client: missing or malformed 'command' field");
        response_body = make_error_response("missing or malformed 'command' field");
    } else {
        spdlog::warn("[uds_server] handle_client: unknown command '{}'", cmd);
        response_body = make_error_response(fmt::format("unknown command '{}'", cmd));
    }

    // ── 응답 송신 ───────────────────────────────────────────────────────
    const uint32_t resp_len = static_cast<uint32_t>(response_body.size());
    const auto resp_hdr = encode_le4(resp_len);

    // 헤더 + 바디를 gather-write
    std::array<asio::const_buffer, 2> bufs{
        asio::buffer(resp_hdr),
        asio::buffer(response_body),
    };
    auto [write_ec, write_n] = co_await asio::async_write(
        socket, bufs, asio::as_tuple(asio::use_awaitable));

    if (write_ec) {
        spdlog::warn("[uds_server] handle_client: write error: {}", write_ec.message());
        co_return;
    }

    spdlog::debug("[uds_server] handled command='{}' response_bytes={}", cmd, write_n);
}
