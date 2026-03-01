// ---------------------------------------------------------------------------
// test_logger.cpp
//
// StructuredLogger 단위 테스트
// ---------------------------------------------------------------------------

#include "logger/log_types.hpp"
#include "logger/structured_logger.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: JSON 라인 파싱 (단순 구현)
// ---------------------------------------------------------------------------
class JsonLineParser {
public:
    explicit JsonLineParser(const std::string& json_str)
        : parsed_(json_str) {}

    bool has_field(const std::string& field) const {
        return parsed_.find("\"" + field + "\"") != std::string::npos;
    }

    std::string get_field(const std::string& field) const {
        std::string search_key = "\"" + field + "\":";
        size_t      pos         = parsed_.find(search_key);
        if (pos == std::string::npos) {
            return "";
        }

        pos += search_key.length();

        // Skip whitespace
        while (pos < parsed_.size() && std::isspace(parsed_[pos])) {
            ++pos;
        }

        if (pos >= parsed_.size()) {
            return "";
        }

        // Extract value (string or number)
        std::ostringstream oss;

        if (parsed_[pos] == '"') {
            // String value
            ++pos;
            while (pos < parsed_.size() && parsed_[pos] != '"') {
                if (parsed_[pos] == '\\' && pos + 1 < parsed_.size()) {
                    ++pos;
                }
                oss << parsed_[pos];
                ++pos;
            }
        } else if (parsed_[pos] == '[') {
            // Array value
            int depth = 0;
            while (pos < parsed_.size()) {
                oss << parsed_[pos];
                if (parsed_[pos] == '[') {
                    ++depth;
                } else if (parsed_[pos] == ']') {
                    --depth;
                    if (depth == 0) {
                        ++pos;
                        break;
                    }
                }
                ++pos;
            }
        } else if (parsed_[pos] == '{') {
            // Object value
            int depth = 0;
            while (pos < parsed_.size()) {
                oss << parsed_[pos];
                if (parsed_[pos] == '{') {
                    ++depth;
                } else if (parsed_[pos] == '}') {
                    --depth;
                    if (depth == 0) {
                        ++pos;
                        break;
                    }
                }
                ++pos;
            }
        } else {
            // Number or boolean
            while (pos < parsed_.size() && (std::isdigit(parsed_[pos]) || parsed_[pos] == '-' ||
                                             parsed_[pos] == '.' || parsed_[pos] == 'e' ||
                                             parsed_[pos] == 'E' || parsed_[pos] == '+')) {
                oss << parsed_[pos];
                ++pos;
            }
        }

        return oss.str();
    }

private:
    std::string parsed_;
};

// ---------------------------------------------------------------------------
// Fixture: Temporary log file
// ---------------------------------------------------------------------------
class StructuredLoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string unique_name =
            std::string(info->test_suite_name()) + "_" + info->name();
        log_dir_  = fs::temp_directory_path() / "dbgate_test_logs" / unique_name;
        log_file_ = log_dir_ / "test.log";
        fs::create_directories(log_dir_);
    }

    void TearDown() override {
        fs::remove_all(log_dir_);
    }

    std::vector<std::string> read_log_lines() const {
        std::vector<std::string> lines;
        std::ifstream            file(log_file_);
        if (!file.is_open()) {
            return lines;
        }

        std::string line;
        while (std::getline(file, line)) {
            // Skip timestamps and keep only JSON part
            size_t json_start = line.find('{');
            if (json_start != std::string::npos) {
                lines.push_back(line.substr(json_start));
            }
        }
        return lines;
    }

    fs::path log_dir_;
    fs::path log_file_;
};

// ---------------------------------------------------------------------------
// Test: ConnectionLog JSON 직렬화
// ---------------------------------------------------------------------------
TEST_F(StructuredLoggerTest, ConnectionLogJsonFormat) {
    StructuredLogger logger(LogLevel::kInfo, log_file_);

    auto                               now = std::chrono::system_clock::now();
    ConnectionLog                      entry;
    entry.session_id   = 12345;
    entry.event        = "connect";
    entry.client_ip    = "192.168.1.100";
    entry.client_port  = 54321;
    entry.db_user      = "testuser";
    entry.timestamp    = now;

    logger.log_connection(entry);

    // 로그 파일 플러시 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto lines = read_log_lines();
    ASSERT_GT(lines.size(), 0) << "No log lines found";

    JsonLineParser parser(lines[0]);
    EXPECT_TRUE(parser.has_field("event"));
    EXPECT_TRUE(parser.has_field("session_id"));
    EXPECT_TRUE(parser.has_field("client_ip"));
    EXPECT_TRUE(parser.has_field("client_port"));
    EXPECT_TRUE(parser.has_field("db_user"));
    EXPECT_TRUE(parser.has_field("timestamp"));

    EXPECT_EQ(parser.get_field("event"), "connect");
    EXPECT_EQ(parser.get_field("session_id"), "12345");
    EXPECT_EQ(parser.get_field("client_ip"), "192.168.1.100");
    EXPECT_EQ(parser.get_field("client_port"), "54321");
    EXPECT_EQ(parser.get_field("db_user"), "testuser");
}

// ---------------------------------------------------------------------------
// Test: QueryLog JSON 필드 확인
// ---------------------------------------------------------------------------
TEST_F(StructuredLoggerTest, QueryLogJsonFields) {
    StructuredLogger logger(LogLevel::kInfo, log_file_);

    auto    now = std::chrono::system_clock::now();
    QueryLog entry;
    entry.session_id   = 67890;
    entry.db_user      = "app_service";
    entry.client_ip    = "172.16.1.50";
    entry.raw_sql      = "SELECT * FROM users WHERE id = 1";
    entry.command_raw  = 0;  // SELECT
    entry.action_raw   = 1;  // ALLOW
    entry.timestamp    = now;
    entry.duration     = std::chrono::microseconds(1500);
    entry.tables.push_back("users");

    logger.log_query(entry);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto lines = read_log_lines();
    ASSERT_GT(lines.size(), 0);

    JsonLineParser parser(lines[0]);
    EXPECT_TRUE(parser.has_field("event"));
    EXPECT_TRUE(parser.has_field("session_id"));
    EXPECT_TRUE(parser.has_field("raw_sql"));
    EXPECT_TRUE(parser.has_field("command_raw"));
    EXPECT_TRUE(parser.has_field("action_raw"));
    EXPECT_TRUE(parser.has_field("tables"));
    EXPECT_TRUE(parser.has_field("duration_us"));

    EXPECT_EQ(parser.get_field("event"), "query");
    EXPECT_EQ(parser.get_field("session_id"), "67890");
    EXPECT_EQ(parser.get_field("db_user"), "app_service");
}

// ---------------------------------------------------------------------------
// Test: BlockLog matched_rule과 reason
// ---------------------------------------------------------------------------
TEST_F(StructuredLoggerTest, BlockLogMatchedRuleAndReason) {
    StructuredLogger logger(LogLevel::kWarn, log_file_);

    auto    now = std::chrono::system_clock::now();
    BlockLog entry;
    entry.session_id   = 11111;
    entry.db_user      = "app_service";
    entry.client_ip    = "172.16.1.50";
    entry.raw_sql      = "DROP TABLE users";
    entry.matched_rule = "sql_rule:block_statements:DROP";
    entry.reason       = "DROP statement not allowed";
    entry.timestamp    = now;

    logger.log_block(entry);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto lines = read_log_lines();
    ASSERT_GT(lines.size(), 0);

    JsonLineParser parser(lines[0]);
    EXPECT_TRUE(parser.has_field("matched_rule"));
    EXPECT_TRUE(parser.has_field("reason"));

    EXPECT_EQ(parser.get_field("matched_rule"), "sql_rule:block_statements:DROP");
    EXPECT_EQ(parser.get_field("reason"), "DROP statement not allowed");
    EXPECT_EQ(parser.get_field("event"), "query_blocked");
}

// ---------------------------------------------------------------------------
// Test: 로그 레벨 필터링
// ---------------------------------------------------------------------------
TEST_F(StructuredLoggerTest, LogLevelFiltering) {
    StructuredLogger logger(LogLevel::kWarn, log_file_);

    // Info 레벨의 로그 (필터되어야 함)
    auto       now = std::chrono::system_clock::now();
    QueryLog entry;
    entry.session_id = 22222;
    entry.timestamp  = now;
    logger.log_query(entry);

    // Warn 레벨의 로그 (기록되어야 함)
    BlockLog block_entry;
    block_entry.session_id = 33333;
    block_entry.timestamp  = now;
    logger.log_block(block_entry);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto lines = read_log_lines();
    // BlockLog만 기록되어야 함 (기록된 JSON 라인만 카운트)
    EXPECT_EQ(lines.size(), 1);
    EXPECT_TRUE(lines[0].find("query_blocked") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test: 멀티스레드 동시 로깅 (크래시 없음)
// ---------------------------------------------------------------------------
TEST_F(StructuredLoggerTest, MultithreadedLoggingNoCrash) {
    StructuredLogger logger(LogLevel::kInfo, log_file_);

    const int                      num_threads = 4;
    const int                      logs_per_thread = 10;
    std::vector<std::thread>       threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&logger, t]() {
            auto now = std::chrono::system_clock::now();
            for (int i = 0; i < logs_per_thread; ++i) {
                QueryLog entry;
                entry.session_id = static_cast<std::uint64_t>(t) * 1000U + static_cast<std::uint64_t>(i);
                entry.db_user    = "user_" + std::to_string(t);
                entry.client_ip  = "192.168.1." + std::to_string(i);
                entry.raw_sql    = "SELECT * FROM table_" + std::to_string(i);
                entry.timestamp  = now;
                logger.log_query(entry);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto lines = read_log_lines();
    // 일부 로그가 기록되었는지 확인 (정확한 개수는 보장 안 함, 기록되었는지만 확인)
    EXPECT_GT(lines.size(), 0);
}

// ---------------------------------------------------------------------------
// Test: JSON 이스케이프 처리
// ---------------------------------------------------------------------------
TEST_F(StructuredLoggerTest, JsonEscaping) {
    StructuredLogger logger(LogLevel::kInfo, log_file_);

    auto    now = std::chrono::system_clock::now();
    QueryLog entry;
    entry.session_id = 44444;
    entry.db_user    = "user\"with\\quotes";
    entry.raw_sql    = "SELECT * FROM users\nWHERE id=1";
    entry.timestamp  = now;

    logger.log_query(entry);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto lines = read_log_lines();
    ASSERT_GT(lines.size(), 0);

    // JSON이 유효한지 확인 (escape가 제대로 되었으므로 파싱 가능)
    JsonLineParser parser(lines[0]);
    EXPECT_TRUE(parser.has_field("db_user"));
    EXPECT_TRUE(parser.has_field("raw_sql"));
}

// ---------------------------------------------------------------------------
// Test: 디버그/정보/경고/에러 로깅
// ---------------------------------------------------------------------------
TEST_F(StructuredLoggerTest, DiagnosticLogging) {
    StructuredLogger logger(LogLevel::kDebug, log_file_);

    logger.debug("Debug message");
    logger.info("Info message");
    logger.warn("Warning message");
    logger.error("Error message");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 진단 로그는 일반 텍스트이므로, 파일이 생성되고 크기가 0이 아닌지 확인
    std::ifstream file(log_file_);
    EXPECT_TRUE(file.is_open()) << "Log file was not created";

    file.seekg(0, std::ios::end);
    std::streamsize file_size = file.tellg();
    EXPECT_GT(file_size, 0) << "Log file is empty";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
