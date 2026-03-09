// ---------------------------------------------------------------------------
// structured_logger.cpp
//
// spdlog 기반 구조화 JSON 로거 구현.
// ---------------------------------------------------------------------------

#include "logger/structured_logger.hpp"

#include <spdlog/common.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Helper: ISO8601 timestamp 포맷
// ---------------------------------------------------------------------------
std::string format_iso8601(const std::chrono::system_clock::time_point& tp) {
    const auto duration = tp.time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration) - seconds;

    const std::time_t time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_val{};
#ifdef _WIN32
    if (gmtime_s(&tm_val, &time_t_val) != 0) {
        return "1970-01-01T00:00:00.000Z";
    }
#else
    if (gmtime_r(&time_t_val, &tm_val) == nullptr) {
        return "1970-01-01T00:00:00.000Z";
    }
#endif

    // strftime + snprintf로 타임스탬프 포맷 (ostringstream 대비 ~10x 빠름)
    std::array<char, 32> date_buf{};
    const auto date_len = std::strftime(date_buf.data(), date_buf.size(),
                                         "%Y-%m-%dT%H:%M:%S", &tm_val);
    if (date_len == 0) {
        return "1970-01-01T00:00:00.000Z";
    }

    std::array<char, 48> buf{};
    (void)std::snprintf(buf.data(),   // NOLINT(cppcoreguidelines-pro-type-vararg)
                        buf.size(),
                        "%.*s.%03dZ",
                        static_cast<int>(date_len),
                        date_buf.data(),
                        static_cast<int>(millis.count()));
    return std::string(buf.data());
}

// ---------------------------------------------------------------------------
// Helper: JSON 문자열 이스케이프 (기본적인 구현)
// ---------------------------------------------------------------------------
std::string escape_json_string(const std::string& str) {
    std::string result;
    result.reserve(str.size() + 16);

    for (const char ch : str) {
        switch (ch) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    std::array<char, 8> buf{};
                    (void)std::snprintf(buf.data(),  // NOLINT(cppcoreguidelines-pro-type-vararg)
                                        buf.size(),
                                        "\\u%04x",
                                        static_cast<unsigned int>(static_cast<unsigned char>(ch)));
                    result += buf.data();
                } else {
                    result += ch;
                }
                break;
        }
    }

    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Helper: spdlog 로그 레벨 변환
// ---------------------------------------------------------------------------
int StructuredLogger::to_spdlog_level(  // NOLINT(readability-convert-member-functions-to-static)
    LogLevel level) const {
    switch (level) {
        case LogLevel::kDebug:
            return static_cast<int>(spdlog::level::debug);
        case LogLevel::kInfo:
            return static_cast<int>(spdlog::level::info);
        case LogLevel::kWarn:
            return static_cast<int>(spdlog::level::warn);
        case LogLevel::kError:
            return static_cast<int>(spdlog::level::err);
        default:
            return static_cast<int>(spdlog::level::info);
    }
}

// ---------------------------------------------------------------------------
// StructuredLogger 생성자
// ---------------------------------------------------------------------------
StructuredLogger::StructuredLogger(
    LogLevel min_level,
    const std::filesystem::path& log_path)  // NOLINT(modernize-pass-by-value)
    : min_level_(min_level), log_path_(log_path) {
    try {
        // 로그 디렉터리 생성
        std::filesystem::create_directories(log_path_.parent_path());

        // 싱크 생성: stdout + rotating file
        std::vector<spdlog::sink_ptr> sinks;

        // Stdout sink
        auto stdout_sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
        sinks.push_back(stdout_sink);

        // Rotating file sink (100MB, 3개 파일 유지)
        const size_t max_file_size = std::size_t{100} * 1024 * 1024;  // 100MB
        const size_t max_files = 3;
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_path_.string(), max_file_size, max_files);
        sinks.push_back(file_sink);

        // 로거 생성 (스레드 안전)
        logger_ = std::make_shared<spdlog::logger>("dbgate", sinks.begin(), sinks.end());
        logger_->set_level(static_cast<spdlog::level::level_enum>(to_spdlog_level(min_level)));

        // 기본 패턴: 타임스탬프만 (구조화 로그는 각 메서드에서 JSON으로 생성)
        logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");

        // warn 이상 로그만 즉시 flush (info 수준은 버퍼에 축적)
        logger_->flush_on(spdlog::level::warn);

        spdlog::register_logger(logger_);

    } catch (const spdlog::spdlog_ex& ex) {
        // 로거 초기화 실패 시, 최소한의 에러 처리
        throw std::runtime_error(std::string("Logger initialization failed: ") + ex.what());
    }
}

StructuredLogger::~StructuredLogger() {
    try {
        if (logger_) {
            logger_->flush();
            spdlog::drop("dbgate");
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Destructor must not throw — swallow all exceptions from spdlog cleanup
    }
}

// ---------------------------------------------------------------------------
// log_connection: JSON 직렬화
// ---------------------------------------------------------------------------
void StructuredLogger::log_connection(const ConnectionLog& entry) {
    if (!logger_ || static_cast<int>(min_level_) > static_cast<int>(LogLevel::kInfo)) {
        return;
    }

    std::string json;
    json.reserve(256);

    json += R"({"event":")";
    json += escape_json_string(entry.event);
    json += R"(","session_id":)";
    json += std::to_string(entry.session_id);
    json += R"(,"client_ip":")";
    json += escape_json_string(entry.client_ip);
    json += R"(","client_port":)";
    json += std::to_string(entry.client_port);
    json += R"(,"db_user":")";
    json += escape_json_string(entry.db_user);
    json += R"(","timestamp":")";
    json += format_iso8601(entry.timestamp);
    json += R"("})";

    logger_->info(json);
}

// ---------------------------------------------------------------------------
// log_query: JSON 직렬화
// ---------------------------------------------------------------------------
void StructuredLogger::log_query(const QueryLog& entry) {
    if (!logger_ || static_cast<int>(min_level_) > static_cast<int>(LogLevel::kInfo)) {
        return;
    }

    std::string json;
    json.reserve(256 + entry.raw_sql.size());

    json += R"({"event":"query","session_id":)";
    json += std::to_string(entry.session_id);
    json += R"(,"db_user":")";
    json += escape_json_string(entry.db_user);
    json += R"(","client_ip":")";
    json += escape_json_string(entry.client_ip);
    json += R"(","raw_sql":")";
    json += escape_json_string(entry.raw_sql);
    json += R"(","command_raw":)";
    json += std::to_string(static_cast<int>(entry.command_raw));
    json += R"(,"tables":[)";

    for (size_t i = 0; i < entry.tables.size(); ++i) {
        if (i > 0) {
            json += ',';
        }
        json += '"';
        json += escape_json_string(entry.tables[i]);
        json += '"';
    }

    json += R"(],"action_raw":)";
    json += std::to_string(static_cast<int>(entry.action_raw));
    json += R"(,"timestamp":")";
    json += format_iso8601(entry.timestamp);
    json += R"(","duration_us":)";
    json += std::to_string(entry.duration.count());
    json += '}';

    logger_->info(json);
}

// ---------------------------------------------------------------------------
// log_block: JSON 직렬화
// ---------------------------------------------------------------------------
void StructuredLogger::log_block(const BlockLog& entry) {
    if (!logger_ || static_cast<int>(min_level_) > static_cast<int>(LogLevel::kWarn)) {
        return;
    }

    // would_block==true: dry-run 모드에서 차단됐을 것임을 나타냄 (실제 차단 아님)
    const char* event_name = entry.would_block ? "query_would_block" : "query_blocked";
    const char* would_block_val = entry.would_block ? "true" : "false";

    std::string json;
    json.reserve(256 + entry.raw_sql.size());

    json += R"({"event":")";
    json += event_name;
    json += R"(","session_id":)";
    json += std::to_string(entry.session_id);
    json += R"(,"db_user":")";
    json += escape_json_string(entry.db_user);
    json += R"(","client_ip":")";
    json += escape_json_string(entry.client_ip);
    json += R"(","raw_sql":")";
    json += escape_json_string(entry.raw_sql);
    json += R"(","matched_rule":")";
    json += escape_json_string(entry.matched_rule);
    json += R"(","reason":")";
    json += escape_json_string(entry.reason);
    json += R"(","would_block":)";
    json += would_block_val;
    json += R"(,"timestamp":")";
    json += format_iso8601(entry.timestamp);
    json += R"("})";

    logger_->warn(json);
}

// ---------------------------------------------------------------------------
// 내부 진단용 spdlog 래퍼
// ---------------------------------------------------------------------------
void StructuredLogger::debug(std::string_view msg) {
    if (logger_) {
        logger_->debug(msg);
    }
}

void StructuredLogger::info(std::string_view msg) {
    if (logger_) {
        logger_->info(msg);
    }
}

void StructuredLogger::warn(std::string_view msg) {
    if (logger_) {
        logger_->warn(msg);
    }
}

void StructuredLogger::error(std::string_view msg) {
    if (logger_) {
        logger_->error(msg);
    }
}
