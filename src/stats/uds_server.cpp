// ---------------------------------------------------------------------------
// uds_server.cpp
//
// UdsServer 구현 — Unix Domain Socket 서버.
// Go CLI/대시보드에 StatsSnapshot, policy_explain, policy_versions,
// policy_rollback, policy_reload 를 노출한다.
//
// [프로토콜]
//   요청/응답 모두 4byte LE 길이 프리픽스 + JSON 바디.
//
// [지원 커맨드]
//   "stats"           — StatsSnapshot JSON 반환
//   "policy_explain"  — SQL 정책 평가 dry-run (DON-48)
//   "policy_versions" — 저장된 정책 버전 목록 조회 (DON-50)
//   "policy_rollback" — 특정 버전으로 정책 롤백 (DON-50)
//   "policy_reload"   — 정책 파일 리로드 + 스냅샷 저장 (DON-50)
//   "sessions"        — 501 placeholder (Phase 3)
//   기타              — error 응답
//
// [격리 원칙]
//   UDS I/O 실패는 데이터패스로 전파하지 않는다.
//   stats 접근은 read-only (StatsCollector::snapshot()) 만 수행한다.
//   policy_explain/policy_rollback/policy_reload 실패는 데이터패스로 전파하지 않는다.
//   롤백/리로드 실패 시 현재 정책을 유지한다 (fail-close).
// ---------------------------------------------------------------------------

#include "stats/uds_server.hpp"

#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "policy/policy_loader.hpp"

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
        R"({{"total_connections":{},"active_sessions":{},"total_queries":{},"blocked_queries":{},"monitored_blocks":{},"qps":{:.4f},"block_rate":{:.4f},"captured_at_ms":{}}})",
        s.total_connections,
        s.active_sessions,
        s.total_queries,
        s.blocked_queries,
        s.monitored_blocks,
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
// current_utc_iso8601
//   현재 UTC 시각을 ISO 8601 형식으로 반환.
//   형식: "2026-03-04T10:35:20Z"
// ---------------------------------------------------------------------------
std::string current_utc_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &time_t_now);
#else
    if (gmtime_r(&time_t_now, &utc_tm) == nullptr) {
        spdlog::warn("[uds_server] current_utc_iso8601: gmtime_r failed, returning epoch");
        return "1970-01-01T00:00:00Z";
    }
#endif

    std::ostringstream oss;
    oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// ---------------------------------------------------------------------------
// safe_strerror
//   thread-safe strerror 래퍼.
// ---------------------------------------------------------------------------
std::string safe_strerror(int errnum) {
    std::array<char, 256> buf{};
#if defined(_WIN32)
    if (strerror_s(buf.data(), buf.size(), errnum) == 0) {
        return std::string{buf.data()};
    }
    return "unknown error";
#elif defined(__GLIBC__)
    const char* msg = strerror_r(errnum, buf.data(), buf.size());
    return std::string{msg != nullptr ? msg : "unknown error"};
#else
    if (strerror_r(errnum, buf.data(), buf.size()) == 0) {
        return std::string{buf.data()};
    }
    return "unknown error";
#endif
}

bool is_json_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// ---------------------------------------------------------------------------
// find_json_string_end
//   json[start_quote]가 '"'일 때, 문자열 리터럴의 닫는 '"' 인덱스를 반환한다.
//   이스케이프(\\, \")를 고려한다. 실패 시 std::string_view::npos 반환.
// ---------------------------------------------------------------------------
std::string_view::size_type find_json_string_end(std::string_view json,
                                                 std::string_view::size_type start_quote) {
    if (start_quote >= json.size() || json[start_quote] != '"') {
        return std::string_view::npos;
    }
    bool escaped = false;
    for (std::string_view::size_type i = start_quote + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            return i;
        }
    }
    return std::string_view::npos;
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
// unescape_json_string
//   JSON 문자열 값의 이스케이프를 처리하여 unescaped 값을 반환한다.
// ---------------------------------------------------------------------------
std::string unescape_json_string(std::string_view escaped_value) {
    std::string result;
    for (std::size_t i = 0; i < escaped_value.size(); ++i) {
        const char c = escaped_value[i];
        if (c == '\\' && i + 1 < escaped_value.size()) {
            const char next = escaped_value[++i];
            switch (next) {
                case '"':
                    result += '"';
                    break;
                case '\\':
                    result += '\\';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 't':
                    result += '\t';
                    break;
                default:
                    result += next;
                    break;
            }
        } else {
            result += c;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// parse_top_level_string_field
//   JSON object 문자열의 "최상위" 키에서 문자열 값을 추출한다.
//   문자열 리터럴 내부의 "key": 패턴은 무시한다 (depth tracking 사용).
//   파싱 실패 시 std::nullopt 반환.
// ---------------------------------------------------------------------------
std::optional<std::string> parse_top_level_string_field(std::string_view object_json,
                                                        std::string_view field_key) {
    if (object_json.empty() || object_json.front() != '{') {
        return std::nullopt;
    }

    std::size_t object_depth = 0;
    std::size_t array_depth = 0;
    for (std::string_view::size_type i = 0; i < object_json.size();) {
        const char c = object_json[i];
        if (c == '{') {
            ++object_depth;
            ++i;
            continue;
        }
        if (c == '}') {
            if (object_depth == 0) {
                return std::nullopt;
            }
            --object_depth;
            ++i;
            continue;
        }
        if (c == '[') {
            ++array_depth;
            ++i;
            continue;
        }
        if (c == ']') {
            if (array_depth == 0) {
                return std::nullopt;
            }
            --array_depth;
            ++i;
            continue;
        }
        if (c == '"') {
            const auto string_end = find_json_string_end(object_json, i);
            if (string_end == std::string_view::npos) {
                return std::nullopt;
            }

            // object_depth == 1 && array_depth == 0 이고 문자열 뒤에 ':'가 오면 key 위치다.
            if (object_depth == 1 && array_depth == 0) {
                auto cursor = string_end + 1;
                while (cursor < object_json.size() && is_json_whitespace(object_json[cursor])) {
                    ++cursor;
                }
                if (cursor < object_json.size() && object_json[cursor] == ':') {
                    const auto key =
                        object_json.substr(i + 1, static_cast<std::size_t>(string_end - i - 1));
                    if (key == field_key) {
                        ++cursor;  // ':'를 건넘
                        while (cursor < object_json.size() &&
                               is_json_whitespace(object_json[cursor])) {
                            ++cursor;
                        }
                        // value가 문자열이어야 함
                        if (cursor < object_json.size() && object_json[cursor] == '"') {
                            const auto val_end = find_json_string_end(object_json, cursor);
                            if (val_end == std::string_view::npos) {
                                return std::nullopt;
                            }
                            auto after_value = val_end + 1;
                            while (after_value < object_json.size() &&
                                   is_json_whitespace(object_json[after_value])) {
                                ++after_value;
                            }
                            if (after_value < object_json.size() && object_json[after_value] != ',' &&
                                object_json[after_value] != '}') {
                                return std::nullopt;
                            }
                            // 문자열 값 추출 및 unescape 처리
                            const auto escaped_content =
                                object_json.substr(cursor + 1, static_cast<std::size_t>(val_end - cursor - 1));
                            return unescape_json_string(escaped_content);
                        }
                        return std::nullopt;
                    }
                }
            }

            i = string_end + 1;
            continue;
        }

        ++i;
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// parse_command
//   JSON 요청 문자열에서 top-level "command" 필드를 파싱한다.
//   문자열 값 내부의 "command": 패턴은 무시한다 (depth tracking 사용).
//   파싱 실패 시 빈 문자열 반환.
// ---------------------------------------------------------------------------
std::string parse_command(std::string_view json) {
    return parse_top_level_string_field(json, "command").value_or(std::string{});
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
    while (start < json.size() && is_json_whitespace(json[start])) {
        ++start;
    }
    if (start >= json.size() || json[start] != '"') {
        return std::nullopt;
    }
    const auto val_start = start;
    const auto val_end = find_json_string_end(json, val_start);
    if (val_end == std::string_view::npos) {
        return std::nullopt;
    }
    auto cursor = val_end + 1;
    while (cursor < json.size() && is_json_whitespace(json[cursor])) {
        ++cursor;
    }
    if (cursor < json.size() && json[cursor] != ',' && json[cursor] != '}') {
        return std::nullopt;
    }

    const auto escaped_value =
        json.substr(val_start + 1, static_cast<std::size_t>(val_end - val_start - 1));

    return unescape_json_string(escaped_value);
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
// parse_top_level_uint64_field
//   JSON object 문자열의 "최상위" 키에서 uint64 값을 추출한다.
//   문자열 리터럴 내부의 "key": 패턴은 무시한다.
//   파싱 실패 또는 값 범위 초과 시 std::nullopt 반환.
// ---------------------------------------------------------------------------
std::optional<std::uint64_t> parse_top_level_uint64_field(std::string_view object_json,
                                                          std::string_view field_key) {
    if (object_json.empty() || object_json.front() != '{') {
        return std::nullopt;
    }

    std::size_t depth = 0;
    for (std::string_view::size_type i = 0; i < object_json.size();) {
        const char c = object_json[i];
        if (c == '"') {
            const auto string_end = find_json_string_end(object_json, i);
            if (string_end == std::string_view::npos) {
                return std::nullopt;
            }

            // depth == 1 이고 문자열 뒤에 ':'가 오면 key 위치다.
            if (depth == 1) {
                auto cursor = string_end + 1;
                while (cursor < object_json.size() && is_json_whitespace(object_json[cursor])) {
                    ++cursor;
                }
                if (cursor < object_json.size() && object_json[cursor] == ':') {
                    const auto key =
                        object_json.substr(i + 1, static_cast<std::size_t>(string_end - i - 1));
                    if (key == field_key) {
                        ++cursor;  // ':'
                        while (cursor < object_json.size() &&
                               is_json_whitespace(object_json[cursor])) {
                            ++cursor;
                        }
                        if (cursor >= object_json.size() || object_json[cursor] < '0' ||
                            object_json[cursor] > '9') {
                            return std::nullopt;
                        }

                        std::uint64_t value = 0;
                        while (cursor < object_json.size() && object_json[cursor] >= '0' &&
                               object_json[cursor] <= '9') {
                            const auto digit =
                                static_cast<std::uint64_t>(object_json[cursor] - '0');
                            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
                                return std::nullopt;
                            }
                            value = value * 10U + digit;
                            ++cursor;
                        }

                        while (cursor < object_json.size() &&
                               is_json_whitespace(object_json[cursor])) {
                            ++cursor;
                        }
                        if (cursor < object_json.size() && object_json[cursor] != ',' &&
                            object_json[cursor] != '}') {
                            return std::nullopt;
                        }
                        return value;
                    }
                }
            }

            i = string_end + 1;
            continue;
        }

        if (c == '{') {
            ++depth;
            ++i;
            continue;
        }
        if (c == '}') {
            if (depth == 0) {
                return std::nullopt;
            }
            --depth;
            ++i;
            continue;
        }

        ++i;
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// extract_top_level_object_field
//   JSON object 문자열의 최상위에서 field_key에 해당하는 object 값을 추출한다.
//   문자열 리터럴 내부의 "field_key": 패턴은 무시한다.
// ---------------------------------------------------------------------------
std::optional<std::string_view> extract_top_level_object_field(std::string_view object_json,
                                                               std::string_view field_key) {
    if (object_json.empty() || object_json.front() != '{') {
        return std::nullopt;
    }

    std::size_t depth = 0;
    for (std::string_view::size_type i = 0; i < object_json.size();) {
        const char c = object_json[i];
        if (c == '"') {
            const auto string_end = find_json_string_end(object_json, i);
            if (string_end == std::string_view::npos) {
                return std::nullopt;
            }

            if (depth == 1) {
                auto cursor = string_end + 1;
                while (cursor < object_json.size() && is_json_whitespace(object_json[cursor])) {
                    ++cursor;
                }
                if (cursor < object_json.size() && object_json[cursor] == ':') {
                    const auto key =
                        object_json.substr(i + 1, static_cast<std::size_t>(string_end - i - 1));
                    if (key == field_key) {
                        ++cursor;  // ':'
                        while (cursor < object_json.size() &&
                               is_json_whitespace(object_json[cursor])) {
                            ++cursor;
                        }
                        if (cursor >= object_json.size() || object_json[cursor] != '{') {
                            return std::nullopt;
                        }
                        const auto end = find_json_object_end(object_json, cursor);
                        if (end == std::string_view::npos) {
                            return std::nullopt;
                        }
                        return object_json.substr(
                            cursor, static_cast<std::size_t>(end - cursor + 1));
                    }
                }
            }

            i = string_end + 1;
            continue;
        }

        if (c == '{') {
            ++depth;
            ++i;
            continue;
        }
        if (c == '}') {
            if (depth == 0) {
                return std::nullopt;
            }
            --depth;
            ++i;
            continue;
        }

        ++i;
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// parse_payload_string_field
//   요청 JSON 에서 payload 오브젝트 내 특정 문자열 필드를 추출한다.
//   {"command":"...", "payload": {"key": "value", ...}} 패턴을 처리한다.
//   payload 키를 찾지 못하거나 필드가 없으면 std::nullopt 반환.
// ---------------------------------------------------------------------------
std::optional<std::string> parse_payload_string_field(std::string_view json,
                                                      std::string_view field_key) {
    const auto payload_json = extract_top_level_object_field(json, "payload");
    if (!payload_json) {
        return std::nullopt;
    }
    return parse_string_field(*payload_json, field_key);
}

// ---------------------------------------------------------------------------
// parse_payload_uint64_field
//   요청 JSON 에서 payload 오브젝트 "최상위"의 uint64 필드를 추출한다.
//   문자열 내부/중첩 오브젝트의 "key": 패턴은 무시한다.
// ---------------------------------------------------------------------------
std::optional<std::uint64_t> parse_payload_uint64_field(std::string_view json,
                                                        std::string_view field_key) {
    const auto payload_json = extract_top_level_object_field(json, "payload");
    if (!payload_json) {
        return std::nullopt;
    }
    return parse_top_level_uint64_field(*payload_json, field_key);
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
    out += R"(,"monitor_mode":)";
    out += result.monitor_mode ? "true" : "false";

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
      version_store_{nullptr},
      policy_config_path_{},
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
      version_store_{nullptr},
      policy_config_path_{},
      ioc_{ioc},
      acceptor_{ioc} {}

// NOLINTNEXTLINE(modernize-pass-by-value)
UdsServer::UdsServer(const std::filesystem::path& socket_path,
                     std::shared_ptr<StatsCollector> stats,
                     std::shared_ptr<PolicyEngine> policy_engine,
                     std::shared_ptr<SqlParser> sql_parser,
                     std::shared_ptr<PolicyVersionStore> version_store,
                     std::filesystem::path policy_config_path,
                     asio::io_context& ioc)
    : socket_path_{socket_path},
      stats_{std::move(stats)},
      policy_engine_{std::move(policy_engine)},
      sql_parser_{std::move(sql_parser)},
      version_store_{std::move(version_store)},
      policy_config_path_{std::move(policy_config_path)},
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
// DON-53: 보안 설정 setter
// ---------------------------------------------------------------------------
void UdsServer::set_client_timeout(std::uint32_t timeout_sec) {
    client_timeout_sec_.store(timeout_sec, std::memory_order_relaxed);
}

void UdsServer::set_max_connections(std::uint32_t max_conn) {
    max_connections_.store(max_conn, std::memory_order_relaxed);
}

void UdsServer::set_allowed_uid(uid_t uid) {
    allowed_uid_.store(uid, std::memory_order_relaxed);
    allowed_uid_set_.store(true, std::memory_order_release);
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

    // DON-53: 소켓 파일 권한 0600 고정 (fail-close)
    if (::chmod(socket_path_.c_str(), 0600) != 0) {
        const int chmod_errno = errno;
        spdlog::error("[uds_server] chmod(0600) failed on {}: {}",
                      socket_path_.string(),
                      safe_strerror(chmod_errno));
        co_return;
    }

    // 권한 설정 검증
    struct stat st {};  // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
    if (::stat(socket_path_.c_str(), &st) == 0) {
        const auto actual_perms = st.st_mode & 0777;
        if (actual_perms != 0600) {
            spdlog::warn("[uds_server] socket permissions may not be writable due to mount options: "
                         "requested 0600 but got 0{:o} on {}",
                         actual_perms, socket_path_.string());
        } else {
            spdlog::info("[uds_server] socket permissions verified as 0600: {}", socket_path_.string());
        }
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

        // DON-53: 동시 연결 수 제한
        const auto current = active_connections_.load(std::memory_order_relaxed);
        const auto max_connections = max_connections_.load(std::memory_order_relaxed);
        if (current >= max_connections) {
            spdlog::warn("[uds_server] max control connections ({}) reached, rejecting",
                         max_connections);
            boost::system::error_code close_ec;
            client_socket.close(close_ec);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
            continue;
        }

        // 클라이언트 처리 코루틴을 독립적으로 spawn (실패가 서버에 영향 없음)
        active_connections_.fetch_add(1, std::memory_order_relaxed);
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
    // DON-53: RAII guard for active connection count
    class ConnectionGuard final {
    public:
        explicit ConnectionGuard(std::atomic<std::uint32_t>* counter) noexcept : counter_{counter} {}
        ~ConnectionGuard() {
            if (counter_ != nullptr) {
                counter_->fetch_sub(1, std::memory_order_relaxed);
            }
        }

        ConnectionGuard(const ConnectionGuard&) = delete;
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;
        ConnectionGuard(ConnectionGuard&&) = delete;
        ConnectionGuard& operator=(ConnectionGuard&&) = delete;

    private:
        std::atomic<std::uint32_t>* counter_{nullptr};
    };
    const ConnectionGuard conn_guard{&active_connections_};
    auto socket_sp = std::make_shared<asio::local::stream_protocol::socket>(std::move(socket));
    auto& client_socket = *socket_sp;

    // DON-53: SO_PEERCRED 검증 — 허용 UID 외 차단
    {
        const bool uid_is_set = allowed_uid_set_.load(std::memory_order_acquire);
        const uid_t expected_uid =
            uid_is_set ? allowed_uid_.load(std::memory_order_relaxed) : ::getuid();
        struct ucred peer_cred {};
        socklen_t cred_len = sizeof(peer_cred);
        if (::getsockopt(
                client_socket.native_handle(), SOL_SOCKET, SO_PEERCRED, &peer_cred, &cred_len) != 0) {
            const int getsockopt_errno = errno;
            spdlog::warn("[uds_server] SO_PEERCRED failed: {}", safe_strerror(getsockopt_errno));
            co_return;
        }
        if (peer_cred.uid != expected_uid) {
            spdlog::warn("[uds_server] peer UID {} rejected (expected {})",
                         peer_cred.uid,
                         expected_uid);
            co_return;
        }
    }

    const auto io_executor = client_socket.get_executor();

    // DON-53: 읽기 타임아웃용 타이머
    asio::steady_timer deadline{io_executor};
    deadline.expires_after(
        std::chrono::seconds(client_timeout_sec_.load(std::memory_order_relaxed)));
    deadline.async_wait([socket_sp](boost::system::error_code ec) {
        if (!ec) {
            // 타임아웃 발생 — 소켓을 닫아 진행 중인 async_read 를 취소한다
            boost::system::error_code close_ec;
            socket_sp->close(close_ec);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
        }
    });

    // ── 요청 헤더 읽기 ──────────────────────────────────────────────────
    std::array<uint8_t, 4> req_hdr{};
    auto [hdr_ec, hdr_n] = co_await asio::async_read(
        client_socket, asio::buffer(req_hdr), asio::as_tuple(asio::use_awaitable));

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
        client_socket, asio::buffer(body_buf), asio::as_tuple(asio::use_awaitable));

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
    } else if (cmd == "policy_versions") {
        // policy_versions — 저장된 정책 버전 목록 조회 (DON-50)
        response_body = handle_policy_versions(request_json);
    } else if (cmd == "policy_rollback") {
        // policy_rollback — 특정 버전으로 정책 롤백 (DON-50)
        // 파일 I/O/파싱이 포함되므로 control_pool_에서 실행해 이벤트 루프 블로킹을 방지한다.
        co_await asio::dispatch(control_pool_.get_executor(), asio::use_awaitable);
        response_body = handle_policy_rollback(request_json);
        co_await asio::dispatch(io_executor, asio::use_awaitable);
    } else if (cmd == "policy_reload") {
        // policy_reload — 정책 파일 리로드 + 스냅샷 저장 (DON-50)
        // 파일 I/O/해시 계산이 포함되므로 control_pool_에서 실행해 이벤트 루프 블로킹을 방지한다.
        co_await asio::dispatch(control_pool_.get_executor(), asio::use_awaitable);
        response_body = handle_policy_reload(request_json);
        co_await asio::dispatch(io_executor, asio::use_awaitable);
    } else if (cmd == "sessions") {
        // Phase 3 구현 예정
        response_body = make_not_implemented_response("sessions");
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
        co_await asio::async_write(client_socket, bufs, asio::as_tuple(asio::use_awaitable));

    if (write_ec) {
        spdlog::warn("[uds_server] handle_client: write error: {}", write_ec.message());
        co_return;
    }

    // DON-53: 정상 완료 — 타이머 취소
    deadline.cancel();

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

// ---------------------------------------------------------------------------
// handle_policy_versions (DON-50)
//   저장된 정책 버전 목록 및 현재 활성 버전을 반환한다.
//
//   [응답 형식]
//   {"ok":true,"payload":{"current":5,"versions":[{"version":5,"timestamp":"...","rules_count":24,"hash":"abc123..."},...]}}
//
//   [fail-close]
//   version_store_ 미주입 시 not-implemented 반환.
//   예외 발생 시 ok:false 반환 (데이터패스 비전파).
// ---------------------------------------------------------------------------
std::string UdsServer::handle_policy_versions(std::string_view /*request_json*/) {
    if (!version_store_ || !policy_engine_) {
        spdlog::warn("[uds_server] policy_versions: version_store or policy_engine not configured");
        return make_not_implemented_response("policy_versions");
    }

    try {
        const auto versions = version_store_->list_versions();
        const std::uint64_t current = policy_engine_->current_version();

        // 수동 JSON 직렬화
        std::string payload;
        payload += '{';
        payload += fmt::format(R"("current":{})", current);
        payload += R"(,"versions":[)";

        bool first = true;
        for (const auto& meta : versions) {
            if (!first) {
                payload += ',';
            }
            payload += '{';
            payload += fmt::format(R"("version":{})", meta.version);
            payload += R"(,"timestamp":")" + json_escape(meta.timestamp) + '"';
            payload += fmt::format(R"(,"rules_count":{})", meta.rules_count);
            payload += R"(,"hash":")" + json_escape(meta.hash) + '"';
            payload += '}';
            first = false;
        }

        payload += "]}";

        spdlog::debug(
            "[uds_server] policy_versions: current={} count={}", current, versions.size());
        return make_ok_response(payload);
    } catch (const std::exception& e) {
        spdlog::error("[uds_server] policy_versions: exception: {}", e.what());
        return make_error_response("internal error during policy_versions");
    } catch (...) {
        spdlog::error("[uds_server] policy_versions: unknown exception");
        return make_error_response("internal error during policy_versions");
    }
}

// ---------------------------------------------------------------------------
// handle_policy_rollback (DON-50)
//   payload 에서 target_version 을 파싱하여 해당 스냅샷으로 정책을 롤백한다.
//
//   [응답 형식 (성공)]
//   {"ok":true,"payload":{"rolled_back_to":3,"previous_version":5,"rules_count":20}}
//
//   [응답 형식 (실패)]
//   {"ok":false,"error":"version 3 not found"}
//
//   [fail-close]
//   - version_store_ 또는 policy_engine_ 미주입 시 not-implemented 반환.
//   - target_version 파싱 실패 시 ok:false 반환.
//   - load_snapshot 실패 시 현재 정책 유지 + ok:false 반환.
//   - reload 실패 시 현재 정책 유지 + ok:false 반환 (예외 처리).
// ---------------------------------------------------------------------------
std::string UdsServer::handle_policy_rollback(std::string_view request_json) {
    if (!version_store_ || !policy_engine_) {
        spdlog::warn("[uds_server] policy_rollback: version_store or policy_engine not configured");
        return make_not_implemented_response("policy_rollback");
    }

    // ── payload 오브젝트 내 target_version 파싱 ──────────────────────────
    // 프로토콜: {"command":"policy_rollback","version":1,"payload":{"target_version":3}}
    // payload 최상위 키만 허용한다 (문자열 리터럴/중첩 오브젝트 내 패턴 무시).
    const auto target_version_opt = parse_payload_uint64_field(request_json, "target_version");
    if (!target_version_opt) {
        spdlog::warn("[uds_server] policy_rollback: missing target_version in payload");
        return make_error_response("missing required field: target_version");
    }

    const std::uint64_t target_version = *target_version_opt;
    const std::uint64_t previous_version = policy_engine_->current_version();

    // ── 스냅샷 로드 ──────────────────────────────────────────────────────
    auto load_result = version_store_->load_snapshot(target_version);
    if (!load_result) {
        spdlog::warn("[uds_server] policy_rollback: {}", load_result.error());
        return make_error_response(load_result.error());
    }

    auto new_config = std::make_shared<PolicyConfig>(std::move(*load_result));
    const auto rules_count = static_cast<std::uint32_t>(new_config->access_control.size());

    // ── 정책 엔진 교체 (fail-close: 실패 시 현재 정책 유지) ──────────────
    try {
        policy_engine_->reload(new_config, target_version);
    } catch (const std::exception& e) {
        spdlog::error("[uds_server] policy_rollback: reload exception: {}", e.what());
        return make_error_response(fmt::format("rollback failed during reload: {}", e.what()));
    }

    spdlog::info("[uds_server] policy_rollback: rolled back to v{} (prev=v{})",
                 target_version,
                 previous_version);

    std::string payload;
    payload += '{';
    payload += fmt::format(R"("rolled_back_to":{})", target_version);
    payload += fmt::format(R"(,"previous_version":{})", previous_version);
    payload += fmt::format(R"(,"rules_count":{})", rules_count);
    payload += '}';
    return make_ok_response(payload);
}

// ---------------------------------------------------------------------------
// handle_policy_reload (DON-50, 기존 501 placeholder 교체)
//   정책 파일을 리로드하고 스냅샷을 저장한 뒤 정책 엔진을 교체한다.
//
//   [응답 형식 (성공)]
//   {"ok":true,"payload":{"reloaded_at":"2026-03-04T10:35:20Z","rules_count":24,"version":6,"message":"Policy
//   reloaded successfully"}}
//
//   [응답 형식 (실패)]
//   {"ok":false,"error":"..."}
//
//   [fail-close]
//   - version_store_ 또는 policy_engine_ 미주입 시 not-implemented 반환.
//   - policy_config_path_ 비어있으면 ok:false 반환.
//   - PolicyLoader::load 실패 시 현재 정책 유지 + ok:false 반환.
//   - save_snapshot 실패는 경고 로그만 출력하고 reload 는 계속 진행
//     (스냅샷 저장 실패가 정책 리로드를 막지 않도록 격리).
//   - policy_engine_->reload 예외 시 ok:false 반환 (현재 정책 유지).
// ---------------------------------------------------------------------------
std::string UdsServer::handle_policy_reload(std::string_view /*request_json*/) {
    if (!version_store_ || !policy_engine_) {
        spdlog::warn("[uds_server] policy_reload: version_store or policy_engine not configured");
        return make_not_implemented_response("policy_reload");
    }

    if (policy_config_path_.empty()) {
        spdlog::warn("[uds_server] policy_reload: policy_config_path not configured");
        return make_error_response("policy config path not configured");
    }

    // ── 정책 파일 로드 ────────────────────────────────────────────────────
    auto load_result = PolicyLoader::load(policy_config_path_);
    if (!load_result) {
        spdlog::warn("[uds_server] policy_reload: load failed (keeping current policy): {}",
                     load_result.error());
        return make_error_response(load_result.error());
    }

    auto new_config = std::make_shared<PolicyConfig>(std::move(*load_result));
    const auto rules_count = static_cast<std::uint32_t>(new_config->access_control.size());

    // ── 스냅샷 저장 (격리: 실패해도 리로드 진행) ─────────────────────────
    std::uint64_t new_version = 0;
    auto save_result = version_store_->save_snapshot(*new_config, policy_config_path_);
    if (!save_result) {
        spdlog::warn("[uds_server] policy_reload: snapshot save failed (reload continues): {}",
                     save_result.error());
        // 저장 실패 시 현재 version_store의 next_version - 1 이 아닌 기존 버전 유지
        new_version = version_store_->current_version();
    } else {
        new_version = save_result->version;
    }

    // ── 정책 엔진 교체 (fail-close: 실패 시 현재 정책 유지) ──────────────
    try {
        policy_engine_->reload(new_config, new_version);
    } catch (const std::exception& e) {
        spdlog::error("[uds_server] policy_reload: reload exception: {}", e.what());
        return make_error_response(fmt::format("reload failed during engine update: {}", e.what()));
    }

    const std::string reloaded_at = current_utc_iso8601();

    spdlog::info("[uds_server] policy_reload: reloaded v{} rules={} at {}",
                 new_version,
                 rules_count,
                 reloaded_at);

    std::string payload;
    payload += '{';
    payload += R"("reloaded_at":")" + json_escape(reloaded_at) + '"';
    payload += fmt::format(R"(,"rules_count":{})", rules_count);
    payload += fmt::format(R"(,"version":{})", new_version);
    payload += R"(,"message":"Policy reloaded successfully")";
    payload += '}';
    return make_ok_response(payload);
}
