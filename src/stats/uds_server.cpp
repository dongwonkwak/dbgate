// ---------------------------------------------------------------------------
// uds_server.cpp
//
// UdsServer 구현 — Unix Domain Socket 서버.
// Go CLI/대시보드에 StatsSnapshot 및 policy_explain 을 노출한다.
//
// [프로토콜]
//   요청/응답 모두 4byte LE 길이 프리픽스 + JSON 바디.
//
// [지원 커맨드]
//   "stats"          — StatsSnapshot JSON 반환
//   "policy_explain" — SQL 정책 평가 dry-run (DON-48)
//   "sessions"       — 501 placeholder (Phase 3)
//   "policy_reload"  — 501 placeholder (Phase 3)
//   기타             — error 응답
//
// [격리 원칙]
//   UDS I/O 실패는 데이터패스로 전파하지 않는다.
//   stats 접근은 read-only (StatsCollector::snapshot()) 만 수행한다.
//   policy_explain 실패(파싱 오류 포함)는 데이터패스로 전파하지 않는다.
// ---------------------------------------------------------------------------

#include "stats/uds_server.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
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
    for (const char c : sv) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::array<char, 8> buf{};
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
                    (void)snprintf(buf.data(),
                                   buf.size(),
                                   "\\u%04x",
                                   static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf.data();
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
    const auto epoch_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(s.captured_at.time_since_epoch())
            .count();

    return fmt::format(
        R"({{"total_connections":{},"active_sessions":{},"total_queries":{},"blocked_queries":{},"qps":{:.4f},"block_rate":{:.4f},"captured_at_ms":{}}})",
        s.total_connections,
        s.active_sessions,
        s.total_queries,
        s.blocked_queries,
        s.qps,
        s.block_rate,
        epoch_ms);
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
    return fmt::format(R"({{"ok":false,"error":"not implemented","code":501,"command":"{}"}})",
                       json_escape(cmd));
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
    return static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
           (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
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
    while (start < json.size() && json[start] == ' ') {
        ++start;
    }
    if (start >= json.size() || json[start] != '"') {
        return {};
    }
    ++start;  // 여는 따옴표 건너뜀
    std::string cmd;
    while (start < json.size()) {
        const char c = json[start++];
        if (c == '"') {
            break;
        }
        if (c == '\\' && start < json.size()) {
            cmd += json[start++];  // 단순 escape 처리
        } else {
            cmd += c;
        }
    }
    return cmd;
}

// 단일 클라이언트에서 수신할 최대 메시지 크기 (4MiB)
constexpr uint32_t kMaxRequestSize = 4U * 1024U * 1024U;

// ---------------------------------------------------------------------------
// parse_string_field
//   JSON 문자열에서 특정 키의 문자열 값을 추출한다.
//   "key":"value" 패턴만 지원. 중첩 객체/배열 미지원.
//   파싱 실패 시 std::nullopt 반환.
// ---------------------------------------------------------------------------
std::optional<std::string> parse_string_field(std::string_view json, std::string_view key) {
    // "key": 형태로 탐색 (따옴표 포함)
    const std::string search_key = fmt::format("\"{}\":", key);
    const auto pos = json.find(search_key);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    auto start = pos + search_key.size();
    // 공백 건너뜀
    while (start < json.size() && json[start] == ' ') {
        ++start;
    }
    if (start >= json.size() || json[start] != '"') {
        return std::nullopt;
    }
    ++start;  // 여는 따옴표 건너뜀
    std::string value;
    while (start < json.size()) {
        const char c = json[start++];
        if (c == '"') {
            break;
        }
        if (c == '\\' && start < json.size()) {
            // 단순 이스케이프 처리
            const char escaped = json[start++];
            switch (escaped) {
                case '"':
                    value += '"';
                    break;
                case '\\':
                    value += '\\';
                    break;
                case 'n':
                    value += '\n';
                    break;
                case 'r':
                    value += '\r';
                    break;
                case 't':
                    value += '\t';
                    break;
                default:
                    value += escaped;
                    break;
            }
        } else {
            value += c;
        }
    }
    return value;
}

// ---------------------------------------------------------------------------
// find_json_object_end
//   object_start(여는 '{' 위치)부터 매칭되는 닫는 '}' 위치를 찾는다.
//   문자열 리터럴/이스케이프를 고려해 문자열 내부의 중괄호는 무시한다.
//   매칭 실패 시 std::string_view::npos 반환.
// ---------------------------------------------------------------------------
std::string_view::size_type find_json_object_end(std::string_view json,
                                                 std::string_view::size_type object_start) {
    if (object_start >= json.size() || json[object_start] != '{') {
        return std::string_view::npos;
    }

    std::size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::string_view::size_type i = object_start; i < json.size(); ++i) {
        const char c = json[i];

        if (in_string) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '{') {
            ++depth;
            continue;
        }
        if (c == '}') {
            if (depth == 0) {
                return std::string_view::npos;
            }
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }

    return std::string_view::npos;
}

// ---------------------------------------------------------------------------
// parse_payload_string_field
//   요청 JSON 에서 payload 오브젝트 내 특정 문자열 필드를 추출한다.
//   {"command":"...", "payload": {"key": "value", ...}} 패턴을 처리한다.
//   payload 키를 찾지 못하거나 필드가 없으면 std::nullopt 반환.
// ---------------------------------------------------------------------------
std::optional<std::string> parse_payload_string_field(std::string_view json,
                                                      std::string_view field_key) {
    constexpr std::string_view payload_key = "\"payload\":";
    const auto payload_pos = json.find(payload_key);
    if (payload_pos == std::string_view::npos) {
        return std::nullopt;
    }
    auto start = payload_pos + payload_key.size();
    // 공백 건너뜀
    while (start < json.size() && json[start] == ' ') {
        ++start;
    }
    if (start >= json.size() || json[start] != '{') {
        return std::nullopt;
    }
    // payload 오브젝트 끝 찾기 (문자열 리터럴 내부 중괄호 무시)
    const auto end = find_json_object_end(json, start);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    const auto payload_json = json.substr(start, end - start + 1);
    return parse_string_field(payload_json, field_key);
}

// ---------------------------------------------------------------------------
// policy_action_to_string
//   PolicyAction → JSON 응답용 문자열 변환.
// ---------------------------------------------------------------------------
std::string_view policy_action_to_string(PolicyAction action) {
    switch (action) {
        case PolicyAction::kAllow:
            return "allow";
        case PolicyAction::kLog:
            return "log";
        case PolicyAction::kBlock:
        default:
            return "block";
    }
}

// ---------------------------------------------------------------------------
// sql_command_name
//   SqlCommand → 문자열 변환 (JSON 응답 parsed_command 필드용).
// ---------------------------------------------------------------------------
std::string_view sql_command_name(SqlCommand cmd) {
    switch (cmd) {
        case SqlCommand::kSelect:
            return "SELECT";
        case SqlCommand::kInsert:
            return "INSERT";
        case SqlCommand::kUpdate:
            return "UPDATE";
        case SqlCommand::kDelete:
            return "DELETE";
        case SqlCommand::kDrop:
            return "DROP";
        case SqlCommand::kTruncate:
            return "TRUNCATE";
        case SqlCommand::kAlter:
            return "ALTER";
        case SqlCommand::kCreate:
            return "CREATE";
        case SqlCommand::kCall:
            return "CALL";
        case SqlCommand::kPrepare:
            return "PREPARE";
        case SqlCommand::kExecute:
            return "EXECUTE";
        case SqlCommand::kUnknown:
        default:
            return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// serialize_explain_result
//   ExplainResult + ParsedQuery(optional) → JSON payload 문자열.
//   parsed_command, parsed_tables 는 파싱 성공 시만 포함.
// ---------------------------------------------------------------------------
std::string serialize_explain_result(const ExplainResult& result, const ParsedQuery* parsed_query) {
    // 고정 필드 직렬화
    std::string out;
    out += '{';
    out += R"("action":")" + std::string(policy_action_to_string(result.action)) + '"';
    out += R"(,"matched_rule":")" + json_escape(result.matched_rule) + '"';
    out += R"(,"reason":")" + json_escape(result.reason) + '"';
    out += R"(,"matched_access_rule":")" + json_escape(result.matched_access_rule) + '"';
    out += R"(,"evaluation_path":")" + json_escape(result.evaluation_path) + '"';

    // 파싱 성공 시 parsed_command, parsed_tables 추가
    if (parsed_query != nullptr) {
        out +=
            R"(,"parsed_command":")" + std::string(sql_command_name(parsed_query->command)) + '"';

        out += R"(,"parsed_tables":[)";
        bool first = true;
        for (const auto& tbl : parsed_query->tables) {
            if (!first) {
                out += ',';
            }
            out += '"' + json_escape(tbl) + '"';
            first = false;
        }
        out += ']';
    }

    out += '}';
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// UdsServer 생성자/소멸자
// ---------------------------------------------------------------------------
// NOLINTNEXTLINE(modernize-pass-by-value)
UdsServer::UdsServer(const std::filesystem::path& socket_path,
                     std::shared_ptr<StatsCollector> stats,
                     asio::io_context& ioc)
    : socket_path_{socket_path},
      stats_{std::move(stats)},
      policy_engine_{nullptr},
      sql_parser_{nullptr},
      ioc_{ioc},
      acceptor_{ioc} {}

// NOLINTNEXTLINE(modernize-pass-by-value)
UdsServer::UdsServer(const std::filesystem::path& socket_path,
                     std::shared_ptr<StatsCollector> stats,
                     std::shared_ptr<PolicyEngine> policy_engine,
                     std::shared_ptr<SqlParser> sql_parser,
                     asio::io_context& ioc)
    : socket_path_{socket_path},
      stats_{std::move(stats)},
      policy_engine_{std::move(policy_engine)},
      sql_parser_{std::move(sql_parser)},
      ioc_{ioc},
      acceptor_{ioc} {}

UdsServer::~UdsServer() {
    try {
        stop();
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Destructor must not throw — swallow all exceptions from stop()
    }
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
        acceptor_.cancel(cancel_ec);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
        if (cancel_ec && cancel_ec != asio::error::bad_descriptor) {
            spdlog::warn("[uds_server] stop: acceptor cancel error: {}", cancel_ec.message());
        }

        boost::system::error_code close_ec;
        acceptor_.close(close_ec);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
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
                      socket_path_.string(),
                      fs_ec.message());
        co_return;
    }

    // acceptor 열기
    boost::system::error_code ec;
    acceptor_.open(stream_protocol(), ec);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
    if (ec) {
        spdlog::error("[uds_server] open error: {}", ec.message());
        co_return;
    }

    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    acceptor_.bind(stream_protocol::endpoint{socket_path_.string()}, ec);
    if (ec) {
        spdlog::error("[uds_server] bind error on {}: {}", socket_path_.string(), ec.message());
        co_return;
    }

    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
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
        auto [accept_ec] =
            co_await acceptor_.async_accept(client_socket, asio::as_tuple(asio::use_awaitable));

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
        asio::co_spawn(ioc_, handle_client(std::move(client_socket)), asio::detached);
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
asio::awaitable<void> UdsServer::handle_client(asio::local::stream_protocol::socket socket) {
    // ── 요청 헤더 읽기 ──────────────────────────────────────────────────
    std::array<uint8_t, 4> req_hdr{};
    auto [hdr_ec, hdr_n] = co_await asio::async_read(
        socket, asio::buffer(req_hdr), asio::as_tuple(asio::use_awaitable));

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
        socket, asio::buffer(body_buf), asio::as_tuple(asio::use_awaitable));

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
    } else if (cmd == "policy_explain") {
        // policy_explain — SQL 정책 평가 dry-run (DON-48)
        response_body = handle_policy_explain(request_json);
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
    const auto resp_len = static_cast<uint32_t>(response_body.size());
    const auto resp_hdr = encode_le4(resp_len);

    // 헤더 + 바디를 gather-write
    const std::array<asio::const_buffer, 2> bufs{
        asio::buffer(resp_hdr),
        asio::buffer(response_body),
    };
    auto [write_ec, write_n] =
        co_await asio::async_write(socket, bufs, asio::as_tuple(asio::use_awaitable));

    if (write_ec) {
        spdlog::warn("[uds_server] handle_client: write error: {}", write_ec.message());
        co_return;
    }

    spdlog::debug("[uds_server] handled command='{}' response_bytes={}", cmd, write_n);
}

// ---------------------------------------------------------------------------
// handle_policy_explain
//   payload 필드 파싱 → SqlParser::parse() → PolicyEngine::explain() 또는
//   explain_error() 호출 → JSON 응답 반환.
//
//   [fail-close]
//   - sql/user/source_ip 누락 → ok:false 반환
//   - policy_engine_ 또는 sql_parser_ nullptr → not-implemented 반환
//   - 예외 발생 → ok:false 반환 (데이터패스 비전파)
// ---------------------------------------------------------------------------
std::string UdsServer::handle_policy_explain(std::string_view request_json) {
    // policy_engine / sql_parser 미주입 시 not-implemented 반환
    if (!policy_engine_ || !sql_parser_) {
        spdlog::warn("[uds_server] policy_explain: policy_engine or sql_parser not configured");
        return make_not_implemented_response("policy_explain");
    }

    // ── payload 필드 파싱 ────────────────────────────────────────────────
    const auto sql_opt = parse_payload_string_field(request_json, "sql");
    const auto user_opt = parse_payload_string_field(request_json, "user");
    const auto source_ip_opt = parse_payload_string_field(request_json, "source_ip");

    if (!sql_opt) {
        spdlog::warn("[uds_server] policy_explain: missing required field: sql");
        return make_error_response("missing required field: sql");
    }
    if (!user_opt) {
        spdlog::warn("[uds_server] policy_explain: missing required field: user");
        return make_error_response("missing required field: user");
    }
    if (!source_ip_opt) {
        spdlog::warn("[uds_server] policy_explain: missing required field: source_ip");
        return make_error_response("missing required field: source_ip");
    }

    const std::string& sql_str = *sql_opt;
    const std::string& user_str = *user_opt;
    const std::string& source_ip_str = *source_ip_opt;

    // ── SessionContext 구성 ──────────────────────────────────────────────
    SessionContext session;
    session.db_user = user_str;
    session.client_ip = source_ip_str;

    // ── SQL 파싱 → explain 호출 ──────────────────────────────────────────
    try {
        const auto parse_result = sql_parser_->parse(sql_str);

        if (parse_result) {
            // 파싱 성공 → explain(query, session)
            const ParsedQuery& query = *parse_result;
            const ExplainResult explain = policy_engine_->explain(query, session);

            spdlog::debug("[uds_server] policy_explain: sql='{}' user='{}' ip='{}' action='{}'",
                          sql_str,
                          user_str,
                          source_ip_str,
                          policy_action_to_string(explain.action));

            return make_ok_response(serialize_explain_result(explain, &query));
        }

        // 파싱 실패 → explain_error(error, session)
        const ParseError& parse_error = parse_result.error();
        const ExplainResult explain = policy_engine_->explain_error(parse_error, session);

        spdlog::debug("[uds_server] policy_explain: parse failed sql='{}' reason='{}'",
                      sql_str,
                      parse_error.message);

        return make_ok_response(serialize_explain_result(explain, nullptr));
    } catch (const std::exception& e) {
        spdlog::error("[uds_server] policy_explain: exception: {}", e.what());
        return make_error_response("internal error during policy evaluation");
    } catch (...) {
        spdlog::error("[uds_server] policy_explain: unknown exception");
        return make_error_response("internal error during policy evaluation");
    }
}
