// ---------------------------------------------------------------------------
// structured_logger.cpp
//
// spdlog 기반 구조화 JSON 로거 구현.
// ---------------------------------------------------------------------------

#include "logger/structured_logger.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

#include <spdlog/common.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

// ---------------------------------------------------------------------------
// Helper: ISO8601 timestamp 포맷
// ---------------------------------------------------------------------------
static std::string format_iso8601(const std::chrono::system_clock::time_point& tp) {
    const auto duration = tp.time_since_epoch();
    const auto seconds  = std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto millis   = std::chrono::duration_cast<std::chrono::milliseconds>(duration) - seconds;

    std::time_t time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm*    tm_val     = std::gmtime(&time_t_val);

    std::ostringstream oss;
    oss << std::put_time(tm_val, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
        << millis.count() << 'Z';
    return oss.str();
}

// ---------------------------------------------------------------------------
// Helper: JSON 문자열 이스케이프 (기본적인 구현)
// ---------------------------------------------------------------------------
static std::string escape_json_string(const std::string& str) {
    std::string result;
    result.reserve(str.size() + 16);

    for (unsigned char ch : str) {
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
                if (ch < 0x20) {
                    char buf[8]{};
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(ch));
                    result += buf;
                } else {
                    result += ch;
                }
                break;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Helper: spdlog 로그 레벨 변환
// ---------------------------------------------------------------------------
int StructuredLogger::to_spdlog_level(LogLevel level) const {
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
StructuredLogger::StructuredLogger(LogLevel min_level, const std::filesystem::path& log_path)
    : min_level_(min_level)
    , log_path_(log_path)
{
    try {
        // 로그 디렉터리 생성
        std::filesystem::create_directories(log_path_.parent_path());

        // 싱크 생성: stdout + rotating file
        std::vector<spdlog::sink_ptr> sinks;

        // Stdout sink
        auto stdout_sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
        sinks.push_back(stdout_sink);

        // Rotating file sink (100MB, 3개 파일 유지)
        const size_t max_file_size   = 100 * 1024 * 1024;  // 100MB
        const size_t max_files       = 3;
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_path_.string(), max_file_size, max_files);
        sinks.push_back(file_sink);

        // 로거 생성 (스레드 안전)
        logger_ = std::make_shared<spdlog::logger>("dbgate", sinks.begin(), sinks.end());
        logger_->set_level(static_cast<spdlog::level::level_enum>(to_spdlog_level(min_level)));

        // 기본 패턴: 타임스탬프만 (구조화 로그는 각 메서드에서 JSON으로 생성)
        logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");

        // 매 로그마다 파일을 플러시하도록 설정
        logger_->flush_on(spdlog::level::trace);

        spdlog::register_logger(logger_);

    } catch (const spdlog::spdlog_ex& ex) {
        // 로거 초기화 실패 시, 최소한의 에러 처리
        throw std::runtime_error(std::string("Logger initialization failed: ") + ex.what());
    }
}

StructuredLogger::~StructuredLogger() {
    try {
        if (logger_) {
            spdlog::drop("dbgate");
        }
    } catch (...) {
        // 소멸자에서 예외 무시
    }
}

// ---------------------------------------------------------------------------
// log_connection: JSON 직렬화
// ---------------------------------------------------------------------------
void StructuredLogger::log_connection(const ConnectionLog& entry) {
    if (!logger_ || static_cast<int>(min_level_) > static_cast<int>(LogLevel::kInfo)) {
        return;
    }

    // JSON 구성
    std::ostringstream json;
    json << R"({"event":")" << escape_json_string(entry.event) << R"(","session_id":)"
         << entry.session_id << R"(,"client_ip":")" << escape_json_string(entry.client_ip)
         << R"(","client_port":)" << entry.client_port << R"(,"db_user":")"
         << escape_json_string(entry.db_user) << R"(","timestamp":")"
         << format_iso8601(entry.timestamp) << R"("})";

    logger_->info(json.str());
}

// ---------------------------------------------------------------------------
// log_query: JSON 직렬화
// ---------------------------------------------------------------------------
void StructuredLogger::log_query(const QueryLog& entry) {
    if (!logger_ || static_cast<int>(min_level_) > static_cast<int>(LogLevel::kInfo)) {
        return;
    }

    // JSON 구성
    std::ostringstream json;
    json << R"({"event":"query","session_id":)" << entry.session_id << R"(,"db_user":")"
         << escape_json_string(entry.db_user) << R"(","client_ip":")"
         << escape_json_string(entry.client_ip) << R"(","raw_sql":")"
         << escape_json_string(entry.raw_sql) << R"(","command_raw":)" << static_cast<int>(entry.command_raw)
         << R"(,"tables":[)";

    for (size_t i = 0; i < entry.tables.size(); ++i) {
        if (i > 0) {
            json << ',';
        }
        json << R"(")" << escape_json_string(entry.tables[i]) << R"(")";
    }

    json << R"(],"action_raw":)" << static_cast<int>(entry.action_raw)
         << R"(,"timestamp":")" << format_iso8601(entry.timestamp)
         << R"(","duration_us":)" << entry.duration.count() << R"(})";

    logger_->info(json.str());
}

// ---------------------------------------------------------------------------
// log_block: JSON 직렬화
// ---------------------------------------------------------------------------
void StructuredLogger::log_block(const BlockLog& entry) {
    if (!logger_ || static_cast<int>(min_level_) > static_cast<int>(LogLevel::kWarn)) {
        return;
    }

    // JSON 구성
    std::ostringstream json;
    json << R"({"event":"query_blocked","session_id":)" << entry.session_id
         << R"(,"db_user":")" << escape_json_string(entry.db_user) << R"(","client_ip":")"
         << escape_json_string(entry.client_ip) << R"(","raw_sql":")"
         << escape_json_string(entry.raw_sql) << R"(","matched_rule":")"
         << escape_json_string(entry.matched_rule) << R"(","reason":")"
         << escape_json_string(entry.reason) << R"(","timestamp":")"
         << format_iso8601(entry.timestamp) << R"("})";

    logger_->warn(json.str());
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
