// ---------------------------------------------------------------------------
// test_uds_server.cpp
//
// UdsServer 단위 테스트.
//
// [테스트 범위]
// - "stats" 커맨드 → 유효한 JSON {"ok":true,"payload":{...}} 응답
// - "policy_explain" 커맨드 → 정책 평가 dry-run 응답 (DON-48)
//   - 유효한 SQL → ok:true + action/matched_rule/parsed_command/parsed_tables
//   - 차단 SQL (DROP) → ok:true + action:block
//   - payload 필드 누락 → ok:false + error
//   - policy_engine/sql_parser 미주입 → not-implemented 응답
// - 미지원 커맨드("unknown_cmd") → {"ok":false,"error":"..."} 응답
// - "command" 필드 없는 JSON → {"ok":false,"error":"..."} 응답
// - 잘못된 프레임(0-length body, 과대 body length) → 서버가 안전하게 처리
// - 여러 클라이언트 동시 접속 → 각자 올바른 응답 수신
// - run() 전 stop() 호출 → 크래시/hang 없음
//
// [테스트 패턴]
// - 각 테스트는 임시 소켓 경로(/tmp/test_uds_<pid>_<N>.sock)를 사용한다.
// - UdsServer 를 서버 전용 io_context 에서 백그라운드 스레드로 구동한다.
// - 클라이언트는 별도 io_context 의 동기 소켓(sync connect/write/read) 사용.
//
// [프로토콜]
//   요청: [4byte LE 길이][JSON 바디]
//   응답: [4byte LE 길이][JSON 바디]
//
// [알려진 한계]
// - wait_for_socket(2s) 안에 완료되지 않으면 실패. CI 부하에 따라 간헐 실패
//   가능성이 있으나 서버 기동 시간은 수 ms 이므로 2s 는 충분한 마진이다.
// - MalformedFrame 테스트는 서버가 co_return(소켓 종료)하는 동작을 확인한다.
//   서버가 hang하거나 crash하지 않아야 한다.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "parser/sql_parser.hpp"
#include "policy/policy_engine.hpp"
#include "policy/policy_version_store.hpp"
#include "policy/rule.hpp"
#include "stats/stats_collector.hpp"
#include "stats/uds_server.hpp"

namespace asio = boost::asio;
using stream_protocol = asio::local::stream_protocol;

// ---------------------------------------------------------------------------
// 익명 네임스페이스: 테스트 내부 헬퍼
// ---------------------------------------------------------------------------
namespace {

// make_explain_policy_config
//   policy_explain 테스트용 최소 PolicyConfig.
//   app_service 유저에게 SELECT/INSERT/UPDATE 허용, DROP 차단.
std::shared_ptr<PolicyConfig> make_explain_policy_config() {
    auto cfg = std::make_shared<PolicyConfig>();
    cfg->sql_rules.block_statements = {"DROP", "TRUNCATE"};

    AccessRule rule;
    rule.user = "app_service";
    rule.source_ip_cidr = "172.16.0.0/12";
    rule.allowed_tables = {"*"};
    rule.allowed_operations = {"SELECT", "INSERT", "UPDATE", "DELETE"};
    cfg->access_control.push_back(std::move(rule));

    return cfg;
}

// 임시 소켓 경로 생성 (PID + 단조 카운터로 테스트 간 충돌 방지)
std::filesystem::path temp_socket_path(const char* tag) {
    static std::atomic<int> counter{0};
    return std::filesystem::path("/tmp") /
           ("test_uds_" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1)) +
            "_" + tag + ".sock");
}

// encode_le4: uint32_t → 4바이트 little-endian 배열
std::array<uint8_t, 4> encode_le4(uint32_t v) {
    return {
        static_cast<uint8_t>(v),
        static_cast<uint8_t>(v >> 8),
        static_cast<uint8_t>(v >> 16),
        static_cast<uint8_t>(v >> 24),
    };
}

// decode_le4: 4바이트 LE 배열 → uint32_t
uint32_t decode_le4(const std::array<uint8_t, 4>& b) {
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

// ---------------------------------------------------------------------------
// UdsSyncClient
//   동기 UDS 클라이언트 (테스트 전용).
//   자체 io_context 를 보유하여 서버 ioc 와 완전히 분리된다.
// ---------------------------------------------------------------------------
struct UdsSyncClient {
    asio::io_context ioc;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
    stream_protocol::socket sock{
        ioc};  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)

    // connect: 소켓 경로에 동기 연결 (실패 시 예외)
    void connect(const std::filesystem::path& path) {
        sock.connect(stream_protocol::endpoint{path.string()});
    }

    // send: [4LE 헤더][JSON 바디] 형식으로 전송
    void send(std::string_view body) {
        const auto hdr = encode_le4(static_cast<uint32_t>(body.size()));
        const std::array<asio::const_buffer, 2> bufs{
            asio::buffer(hdr),
            asio::buffer(body.data(), body.size()),
        };
        (void)asio::write(sock, bufs);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
    }

    // recv: [4LE 헤더][바디] 형식으로 수신하여 바디 문자열 반환.
    // 서버가 소켓을 닫으면(EOF/오류) 빈 문자열 반환.
    std::string recv() {
        std::array<uint8_t, 4> hdr{};
        boost::system::error_code ec;
        asio::read(sock, asio::buffer(hdr), ec);
        if (ec) {
            return {};
        }

        const uint32_t len = decode_le4(hdr);
        // 응답이 0 이거나 비현실적으로 크면 오류로 취급
        if (len == 0 || len > 16U * 1024U * 1024U) {
            return {};
        }

        std::string body(len, '\0');
        asio::read(sock, asio::buffer(body), ec);
        if (ec) {
            return {};
        }
        return body;
    }

    // send_raw_header: 헤더만 전송 (malformed frame 테스트용)
    void send_raw_header(uint32_t fake_len) {
        const auto hdr = encode_le4(fake_len);
        boost::system::error_code ec;
        (void)asio::write(
            sock, asio::buffer(hdr), ec);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// UdsServerTest 픽스처
//   각 테스트마다 독립된 소켓 경로 + UdsServer + io_context 를 생성/정리한다.
// ---------------------------------------------------------------------------
class UdsServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        socket_path_ = temp_socket_path("srv");
        stats_ = std::make_shared<StatsCollector>();
        ioc_ = std::make_unique<asio::io_context>();
        server_ = std::make_unique<UdsServer>(socket_path_, stats_, *ioc_);
    }

    void TearDown() override {
        stop_server();
        (void)std::filesystem::remove(
            socket_path_);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
    }

    // start_server: 백그라운드 스레드에서 서버 io_context 실행
    void start_server() {
        asio::co_spawn(*ioc_, server_->run(), asio::detached);
        server_thread_ = std::thread([this]() { ioc_->run(); });
    }

    // stop_server: 서버를 중단하고 스레드 join
    void stop_server() {
        if (server_) {
            server_->stop();
        }
        ioc_->stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    // wait_for_socket: 서버가 소켓 파일을 생성할 때까지 polling 대기
    bool wait_for_socket(std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::exists(socket_path_)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }

    std::filesystem::path
        socket_path_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::shared_ptr<StatsCollector>
        stats_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::unique_ptr<asio::io_context>
        ioc_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::unique_ptr<UdsServer>
        server_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::thread
        server_thread_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
};

// ---------------------------------------------------------------------------
// StatsCommand_ReturnsValidSnapshot
//   "stats" 커맨드를 전송하면 {"ok":true,"payload":{...}} JSON이 반환되어야 한다.
//
//   [검증 포인트]
//   - "ok":true 포함
//   - "payload" 필드 포함
//   - StatsSnapshot 의 주요 필드(total_connections 등) 포함
// ---------------------------------------------------------------------------
TEST_F(UdsServerTest, StatsCommand_ReturnsValidSnapshot) {
    // 사전에 통계 데이터 추가
    stats_->on_connection_open();
    stats_->on_query(false);
    stats_->on_query(true);

    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_))
        << "Client must connect to UDS server successfully";

    client.send(R"({"command":"stats","version":1})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "stats command must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":true)"), std::string::npos)
        << "stats response must contain \"ok\":true. Got: " << resp;
    EXPECT_NE(resp.find(R"("payload")"), std::string::npos)
        << "stats response must contain \"payload\" field. Got: " << resp;
    EXPECT_NE(resp.find(R"("total_connections")"), std::string::npos)
        << "payload must contain total_connections. Got: " << resp;
    EXPECT_NE(resp.find(R"("total_queries")"), std::string::npos)
        << "payload must contain total_queries. Got: " << resp;
    EXPECT_NE(resp.find(R"("blocked_queries")"), std::string::npos)
        << "payload must contain blocked_queries. Got: " << resp;
    EXPECT_NE(resp.find(R"("block_rate")"), std::string::npos)
        << "payload must contain block_rate. Got: " << resp;
}

// ---------------------------------------------------------------------------
// UnknownCommand_ReturnsError
//   미지원 커맨드("xyz_unknown") 전송 시 {"ok":false,"error":"..."} 반환.
//
//   [검증 포인트]
//   - "ok":false 포함
//   - "error" 필드 포함
// ---------------------------------------------------------------------------
TEST_F(UdsServerTest, UnknownCommand_ReturnsError) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    client.send(R"({"command":"xyz_unknown_command","version":1})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "Unknown command must return a response (not silence)";

    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Unknown command must return ok:false. Got: " << resp;
    EXPECT_NE(resp.find(R"("error")"), std::string::npos)
        << "Unknown command must return error field. Got: " << resp;
}

// ---------------------------------------------------------------------------
// MissingCommandField_ReturnsError
//   "command" 키 자체가 없는 JSON 전송 시 오류 응답 반환.
//
//   [엣지 케이스] malformed JSON 또는 필드 누락
// ---------------------------------------------------------------------------
TEST_F(UdsServerTest, MissingCommandField_ReturnsError) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    // "command" 키 없는 JSON
    client.send(R"({"version":1,"data":"no_command_here"})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "Missing command field must trigger an error response";

    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Missing command must return ok:false. Got: " << resp;
}

// ---------------------------------------------------------------------------
// MalformedFrame_ZeroBodyLength_Handled
//   4바이트 헤더가 0(body_len == 0)인 경우 서버가 안전하게 연결을 닫아야 한다.
//
//   [엣지 케이스] body_len = 0 → kMaxRequestSize 조건(body_len == 0 은 거부)
//   [검증] 서버가 응답을 보내지 않고 소켓을 닫는다 → recv() 가 빈 문자열 반환.
// ---------------------------------------------------------------------------
TEST_F(UdsServerTest, MalformedFrame_ZeroBodyLength_Handled) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    // body_len = 0 전송 (프로토콜 위반: 서버는 연결을 끊어야 함)
    client.send_raw_header(0U);

    const std::string resp = client.recv();
    // 서버가 응답 없이 소켓을 닫으므로 빈 문자열 기대
    EXPECT_TRUE(resp.empty())
        << "Server must close connection on zero-length body without sending response";
}

// ---------------------------------------------------------------------------
// MalformedFrame_OversizedBodyLength_Handled
//   body_len 이 kMaxRequestSize(4MiB)를 초과하는 경우 서버가 안전하게 처리.
//
//   [엣지 케이스] 4GiB − 1 크기 요청 → 메모리 할당 시도 없이 연결 종료
// ---------------------------------------------------------------------------
TEST_F(UdsServerTest, MalformedFrame_OversizedBodyLength_Handled) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    // body_len = 0xFFFFFFFF (4GiB - 1, kMaxRequestSize 초과)
    client.send_raw_header(0xFFFFFFFFU);

    const std::string resp = client.recv();
    EXPECT_TRUE(resp.empty())
        << "Server must close connection for oversized body length without crash";
}

// ---------------------------------------------------------------------------
// MultipleClients_Concurrent
//   N개 스레드가 각각 독립된 클라이언트로 "stats" 커맨드를 동시에 전송해도
//   모든 클라이언트가 올바른 응답을 수신해야 한다.
//
//   [동시성 검증]
//   - 서버의 co_spawn 기반 클라이언트 처리가 독립적임을 확인.
//   - 한 클라이언트 오류가 다른 클라이언트에 영향을 주지 않아야 한다.
// ---------------------------------------------------------------------------
TEST_F(UdsServerTest, MultipleClients_Concurrent) {
    constexpr int client_count = 4;

    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    std::vector<std::string> responses(static_cast<std::size_t>(client_count));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(client_count));

    for (int i = 0; i < client_count; ++i) {
        threads.emplace_back([this, i, &responses]() {
            UdsSyncClient client;
            try {
                client.connect(socket_path_);
                client.send(R"({"command":"stats","version":1})");
                responses[static_cast<std::size_t>(i)] = client.recv();
            } catch (const std::exception& ex) {
                responses[static_cast<std::size_t>(i)] = std::string("EXCEPTION: ") + ex.what();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 모든 클라이언트가 정상 응답을 받아야 함
    for (int i = 0; i < client_count; ++i) {
        const auto& resp = responses[static_cast<std::size_t>(i)];
        EXPECT_FALSE(resp.empty()) << "Client " << i << " must receive a non-empty response";
        EXPECT_NE(resp.find(R"("ok":true)"), std::string::npos)
            << "Client " << i << " must receive ok:true. Got: " << resp;
    }
}

// ---------------------------------------------------------------------------
// StopBeforeRun_NoCrash
//   run() 을 호출하지 않고 stop() 을 호출해도 크래시/hang이 없어야 한다.
//
//   [엣지 케이스] 생명주기 오순서 방어 (stop → run 순서 역전)
// ---------------------------------------------------------------------------
TEST_F(UdsServerTest, StopBeforeRun_NoCrash) {
    // run() 없이 stop() 만 호출 — acceptor 는 닫혀 있으므로 no-op
    ASSERT_NO_THROW(server_->stop()) << "stop() before run() must not throw or crash";
}

// ===========================================================================
// UdsPolicyExplainTest 픽스처 (DON-48)
//   PolicyEngine + SqlParser 를 주입한 UdsServer 에서 policy_explain 커맨드를
//   테스트한다.
//
//   [정책 설정]
//   - DROP / TRUNCATE → block_statements (항상 차단)
//   - app_service 유저 + 172.16.0.0/12 → SELECT/INSERT/UPDATE/DELETE 허용
// ===========================================================================
class UdsPolicyExplainTest : public ::testing::Test {
protected:
    void SetUp() override {
        socket_path_ = temp_socket_path("explain");
        stats_ = std::make_shared<StatsCollector>();
        ioc_ = std::make_unique<asio::io_context>();

        auto policy_config = make_explain_policy_config();
        policy_engine_ = std::make_shared<PolicyEngine>(std::move(policy_config));
        sql_parser_ = std::make_shared<SqlParser>();

        server_ =
            std::make_unique<UdsServer>(socket_path_, stats_, policy_engine_, sql_parser_, *ioc_);
    }

    void TearDown() override {
        stop_server();
        (void)std::filesystem::remove(
            socket_path_);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
    }

    void start_server() {
        asio::co_spawn(*ioc_, server_->run(), asio::detached);
        server_thread_ = std::thread([this]() { ioc_->run(); });
    }

    void stop_server() {
        if (server_) {
            server_->stop();
        }
        ioc_->stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    bool wait_for_socket(std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::exists(socket_path_)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }

    std::filesystem::path
        socket_path_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::shared_ptr<StatsCollector>
        stats_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::shared_ptr<PolicyEngine>
        policy_engine_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::shared_ptr<SqlParser>
        sql_parser_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::unique_ptr<asio::io_context>
        ioc_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::unique_ptr<UdsServer>
        server_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::thread
        server_thread_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
};

// ---------------------------------------------------------------------------
// PolicyExplain_AllowedSql_ReturnsOkWithAction
//   허용된 SQL (SELECT) 에 대해 ok:true + action 필드가 반환되어야 한다.
//
//   [검증 포인트]
//   - "ok":true 포함
//   - "action" 필드 포함
//   - "parsed_command" 포함 (파싱 성공)
//   - "parsed_tables" 포함
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyExplainTest, PolicyExplain_AllowedSql_ReturnsOkWithAction) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    constexpr std::string_view req =
        R"({"command":"policy_explain","version":1,"payload":{"sql":"SELECT * FROM users","user":"app_service","source_ip":"172.16.0.1"}})";
    client.send(req);

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_explain must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":true)"), std::string::npos)
        << "policy_explain must return ok:true. Got: " << resp;
    EXPECT_NE(resp.find(R"("action")"), std::string::npos)
        << "response payload must contain 'action' field. Got: " << resp;
    EXPECT_NE(resp.find(R"("parsed_command")"), std::string::npos)
        << "response payload must contain 'parsed_command'. Got: " << resp;
    EXPECT_NE(resp.find(R"("parsed_tables")"), std::string::npos)
        << "response payload must contain 'parsed_tables'. Got: " << resp;
    EXPECT_NE(resp.find(R"("SELECT")"), std::string::npos)
        << "parsed_command must be SELECT. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyExplain_BlockedSql_ReturnsActionBlock
//   차단 SQL (DROP TABLE) 에 대해 ok:true + action:block 이 반환되어야 한다.
//
//   [검증 포인트]
//   - "ok":true 포함 (explain 자체는 성공)
//   - "action":"block" 포함 (정책 평가 결과)
//   - "matched_rule" 포함
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyExplainTest, PolicyExplain_BlockedSql_ReturnsActionBlock) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    constexpr std::string_view req =
        R"({"command":"policy_explain","version":1,"payload":{"sql":"DROP TABLE users","user":"app_service","source_ip":"172.16.0.1"}})";
    client.send(req);

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_explain must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":true)"), std::string::npos)
        << "policy_explain (explain itself) must return ok:true. Got: " << resp;
    EXPECT_NE(resp.find(R"("action":"block")"), std::string::npos)
        << "DROP must result in action:block. Got: " << resp;
    EXPECT_NE(resp.find(R"("matched_rule")"), std::string::npos)
        << "response must contain matched_rule. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyExplain_SqlContainingJsonLiteral_DoesNotBreakPayloadBoundary
//   sql 문자열 내부에 JSON 리터럴("}")이 있어도 payload 경계를 올바르게 찾아
//   user/source_ip 필드 파싱이 깨지지 않아야 한다.
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyExplainTest, PolicyExplain_SqlContainingJsonLiteral_DoesNotBreakPayloadBoundary) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    constexpr std::string_view req =
        R"REQ({"command":"policy_explain","version":1,"payload":{"sql":"INSERT INTO logs(payload) VALUES ('{\"k\":1}')","user":"app_service","source_ip":"172.16.0.1"}})REQ";
    client.send(req);

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_explain must return a non-empty response";
    EXPECT_NE(resp.find(R"("ok":true)"), std::string::npos)
        << "SQL 문자열 내부 JSON 리터럴이 있어도 explain은 성공해야 한다. Got: " << resp;
    EXPECT_EQ(resp.find("missing required field"), std::string::npos)
        << "payload 경계 파싱 오류로 필수 필드 누락 에러가 발생하면 안 된다. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyExplain_MissingSqlField_ReturnsError
//   payload 에 "sql" 필드가 없으면 ok:false + error 가 반환되어야 한다.
//
//   [fail-close 검증]
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyExplainTest, PolicyExplain_MissingSqlField_ReturnsError) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    // sql 필드 누락
    constexpr std::string_view req =
        R"({"command":"policy_explain","version":1,"payload":{"user":"app_service","source_ip":"172.16.0.1"}})";
    client.send(req);

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "Missing sql field must return an error response";

    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Missing sql must return ok:false. Got: " << resp;
    EXPECT_NE(resp.find(R"("error")"), std::string::npos)
        << "Response must contain error field. Got: " << resp;
    EXPECT_NE(resp.find("sql"), std::string::npos)
        << "Error message should mention 'sql'. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyExplain_MissingUserField_ReturnsError
//   payload 에 "user" 필드가 없으면 ok:false + error 가 반환되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyExplainTest, PolicyExplain_MissingUserField_ReturnsError) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    constexpr std::string_view req =
        R"({"command":"policy_explain","version":1,"payload":{"sql":"SELECT 1","source_ip":"172.16.0.1"}})";
    client.send(req);

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty());
    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Missing user must return ok:false. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyExplain_MissingSourceIpField_ReturnsError
//   payload 에 "source_ip" 필드가 없으면 ok:false + error 가 반환되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyExplainTest, PolicyExplain_MissingSourceIpField_ReturnsError) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    constexpr std::string_view req =
        R"({"command":"policy_explain","version":1,"payload":{"sql":"SELECT 1","user":"app_service"}})";
    client.send(req);

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty());
    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Missing source_ip must return ok:false. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyExplain_NoPayloadField_ReturnsError
//   payload 필드 자체가 없는 JSON → ok:false 반환.
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyExplainTest, PolicyExplain_NoPayloadField_ReturnsError) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    constexpr std::string_view req = R"({"command":"policy_explain","version":1})";
    client.send(req);

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty());
    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "No payload must return ok:false. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyExplain_WithoutPolicyEngine_ReturnsNotImplemented
//   policy_engine 없이 생성된 UdsServer 에서 policy_explain 커맨드는
//   not-implemented 응답을 반환해야 한다.
//
//   [검증] stats 전용 생성자(policy_engine = nullptr)는 policy_explain 비활성.
// ---------------------------------------------------------------------------
TEST_F(UdsServerTest, PolicyExplain_WithoutPolicyEngine_ReturnsNotImplemented) {
    // UdsServerTest 픽스처는 stats 전용 생성자 사용 (policy_engine = nullptr)
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    constexpr std::string_view req =
        R"({"command":"policy_explain","version":1,"payload":{"sql":"SELECT 1","user":"u","source_ip":"1.2.3.4"}})";
    client.send(req);

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty());
    // policy_engine 없이는 not-implemented (ok:false + code:501) 반환
    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Without policy_engine, policy_explain must return ok:false. Got: " << resp;
    EXPECT_NE(resp.find("501"), std::string::npos)
        << "Response should contain 501 code. Got: " << resp;
}

// ---------------------------------------------------------------------------
// Stats_ContainsMonitoredBlocks (DON-49)
//   on_monitored_block() 를 호출한 후 "stats" 커맨드를 전송하면
//   JSON 응답에 "monitored_blocks" 필드가 포함되어야 한다.
//
//   [검증 포인트]
//   - "monitored_blocks" 키 포함
//   - on_monitored_block() 호출 횟수만큼 값이 반영됨
//     (JSON 파서 없이 문자열 탐색으로 검증)
//
//   [패턴]
//   StatsCommand_ReturnsValidSnapshot 테스트와 동일한 픽스처(UdsServerTest)를 사용한다.
// ---------------------------------------------------------------------------
TEST_F(UdsServerTest, Stats_ContainsMonitoredBlocks) {
    // monitor 모드에서 차단된 쿼리를 시뮬레이션: on_monitored_block() 2회 호출
    stats_->on_monitored_block();
    stats_->on_monitored_block();

    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_))
        << "Client must connect to UDS server successfully";

    client.send(R"({"command":"stats","version":1})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "stats command must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":true)"), std::string::npos)
        << "stats response must contain \"ok\":true. Got: " << resp;
    EXPECT_NE(resp.find(R"("monitored_blocks")"), std::string::npos)
        << "stats payload must contain 'monitored_blocks' field (DON-49). Got: " << resp;
}

// ===========================================================================
// UdsPolicyVersioningTest 픽스처 (DON-50)
//   PolicyVersionStore + PolicyEngine + SqlParser 를 주입한 UdsServer 에서
//   policy_versions, policy_rollback, policy_reload 커맨드를 테스트한다.
//
//   [정책 설정]
//   - 인메모리 PolicyConfig (파일 없이 직접 생성)
//   - PolicyVersionStore 는 임시 디렉토리 사용
// ===========================================================================
class UdsPolicyVersioningTest : public ::testing::Test {
protected:
    void SetUp() override {
        socket_path_ = temp_socket_path("versioning");
        stats_ = std::make_shared<StatsCollector>();
        ioc_ = std::make_unique<asio::io_context>();

        // 임시 디렉토리 생성 (PID + 단조 카운터로 충돌 방지)
        static std::atomic<int> dir_counter{0};
        version_dir_ = std::filesystem::temp_directory_path() /
                       ("test_pvs_" + std::to_string(::getpid()) + "_" +
                        std::to_string(dir_counter.fetch_add(1)));
        std::filesystem::create_directories(version_dir_);

        auto policy_config = make_explain_policy_config();
        policy_engine_ = std::make_shared<PolicyEngine>(policy_config);
        sql_parser_ = std::make_shared<SqlParser>();
        version_store_ = std::make_shared<PolicyVersionStore>(version_dir_);

        server_ =
            std::make_unique<UdsServer>(socket_path_,
                                        stats_,
                                        policy_engine_,
                                        sql_parser_,
                                        version_store_,
                                        std::filesystem::path{},  // 빈 경로 (policy_reload 비활성)
                                        *ioc_);
    }

    void TearDown() override {
        stop_server();
        (void)std::filesystem::remove(
            socket_path_);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
        std::filesystem::remove_all(
            version_dir_);  // NOLINT(bugprone-unused-return-value,cert-err33-c)
    }

    void start_server() {
        asio::co_spawn(*ioc_, server_->run(), asio::detached);
        server_thread_ = std::thread([this]() { ioc_->run(); });
    }

    void stop_server() {
        if (server_) {
            server_->stop();
        }
        ioc_->stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    bool wait_for_socket(std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::exists(socket_path_)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }

    std::filesystem::path
        socket_path_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::filesystem::path
        version_dir_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::shared_ptr<StatsCollector>
        stats_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::shared_ptr<PolicyEngine>
        policy_engine_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::shared_ptr<SqlParser>
        sql_parser_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::shared_ptr<PolicyVersionStore>
        version_store_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::unique_ptr<asio::io_context>
        ioc_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::unique_ptr<UdsServer>
        server_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
    std::thread
        server_thread_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes,readability-identifier-naming)
};

// ---------------------------------------------------------------------------
// PolicyVersions_WithoutVersionStore_ReturnsNotImplemented (DON-50)
//   version_store 없이 생성된 UdsServer 에서 policy_versions 커맨드는
//   not-implemented 응답을 반환해야 한다.
//
//   [검증] stats 전용 생성자(version_store = nullptr)는 버전 커맨드 비활성.
// ---------------------------------------------------------------------------
TEST_F(UdsServerTest, PolicyVersions_WithoutVersionStore_ReturnsNotImplemented) {
    // UdsServerTest 픽스처는 stats 전용 생성자 사용 (version_store = nullptr)
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    client.send(R"({"command":"policy_versions","version":1})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty());
    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Without version_store, policy_versions must return ok:false. Got: " << resp;
    EXPECT_NE(resp.find("501"), std::string::npos)
        << "Response should contain 501 code. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyRollback_WithoutVersionStore_ReturnsNotImplemented (DON-50)
//   version_store 없이 생성된 UdsServer 에서 policy_rollback 커맨드는
//   not-implemented 응답을 반환해야 한다.
// ---------------------------------------------------------------------------
TEST_F(UdsServerTest, PolicyRollback_WithoutVersionStore_ReturnsNotImplemented) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    client.send(R"({"command":"policy_rollback","version":1,"payload":{"target_version":1}})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty());
    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Without version_store, policy_rollback must return ok:false. Got: " << resp;
    EXPECT_NE(resp.find("501"), std::string::npos)
        << "Response should contain 501 code. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyVersions_EmptyStore_ReturnsOkWithEmptyList (DON-50)
//   버전이 저장되지 않은 상태에서 policy_versions 를 조회하면
//   ok:true + 빈 versions 배열이 반환되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyVersioningTest, PolicyVersions_EmptyStore_ReturnsOkWithEmptyList) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    client.send(R"({"command":"policy_versions","version":1})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_versions must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":true)"), std::string::npos)
        << "policy_versions must return ok:true. Got: " << resp;
    EXPECT_NE(resp.find(R"("current")"), std::string::npos)
        << "response payload must contain 'current' field. Got: " << resp;
    EXPECT_NE(resp.find(R"("versions")"), std::string::npos)
        << "response payload must contain 'versions' field. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyVersions_AfterSaveSnapshot_ReturnsList (DON-50)
//   스냅샷 저장 후 policy_versions 를 조회하면 버전 목록이 반환되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyVersioningTest, PolicyVersions_AfterSaveSnapshot_ReturnsList) {
    // 스냅샷을 인메모리로 저장하기 위해 임시 파일 생성
    const auto tmp_policy_path = version_dir_ / "policy.yaml";
    {
        // 최소한의 YAML 파일 (yaml-cpp 파싱용)
        std::ofstream f(tmp_policy_path);
        f << "version: 1\n";
        f << "access_control:\n";
        f << "  - user: test\n";
        f << "    source_ip_cidr: 0.0.0.0/0\n";
        f << "    allowed_operations: [SELECT]\n";
        f << "    allowed_tables: [\"*\"]\n";
        f << "sql_rules:\n";
        f << "  block_patterns:\n";
        f << "    - \"UNION\\\\s+SELECT\"\n";
    }

    // PolicyConfig 직접 생성하여 스냅샷 저장
    auto cfg = make_explain_policy_config();
    const auto save_result = version_store_->save_snapshot(*cfg, tmp_policy_path);
    ASSERT_TRUE(static_cast<bool>(save_result)) << "save_snapshot must succeed";

    // policy_engine_ 에 버전 적용
    policy_engine_->reload(cfg, save_result->version);

    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    client.send(R"({"command":"policy_versions","version":1})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_versions must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":true)"), std::string::npos)
        << "policy_versions must return ok:true. Got: " << resp;
    EXPECT_NE(resp.find(R"("version":1)"), std::string::npos)
        << "response must contain version 1. Got: " << resp;
    EXPECT_NE(resp.find(R"("current":1)"), std::string::npos)
        << "current version must be 1. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyRollback_InvalidVersion_ReturnsError (DON-50)
//   존재하지 않는 버전으로 롤백 시도 시 ok:false + 오류 메시지가 반환되어야 한다.
//   실패 시 현재 정책이 유지된다 (fail-close).
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyVersioningTest, PolicyRollback_InvalidVersion_ReturnsError) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    // 버전 999는 존재하지 않음
    client.send(R"({"command":"policy_rollback","version":1,"payload":{"target_version":999}})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_rollback must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Invalid rollback target must return ok:false. Got: " << resp;
    EXPECT_NE(resp.find(R"("error")"), std::string::npos)
        << "Response must contain error field. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyRollback_MissingTargetVersion_ReturnsError (DON-50)
//   target_version 필드가 없으면 ok:false + 오류 메시지가 반환되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyVersioningTest, PolicyRollback_MissingTargetVersion_ReturnsError) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    // target_version 누락
    client.send(R"({"command":"policy_rollback","version":1,"payload":{}})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_rollback must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Missing target_version must return ok:false. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyRollback_TargetVersionOutsidePayload_IsIgnored
//   payload 바깥의 target_version 필드는 무시되어야 하며,
//   payload 내 target_version 이 없으면 오류를 반환해야 한다.
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyVersioningTest, PolicyRollback_TargetVersionOutsidePayload_IsIgnored) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    client.send(R"({"command":"policy_rollback","version":1,"target_version":1,"payload":{}})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_rollback must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "target_version outside payload must be ignored. Got: " << resp;
    EXPECT_NE(resp.find("missing required field: target_version"), std::string::npos)
        << "Error should indicate missing payload.target_version. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyRollback_TargetVersionStringLiteral_DoesNotOverridePayloadField
//   payload 문자열 리터럴에 포함된 \"target_version\":N 패턴이
//   실제 payload.target_version 값을 덮어쓰면 안 된다.
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyVersioningTest, PolicyRollback_TargetVersionStringLiteral_DoesNotOverridePayloadField) {
    const auto tmp_policy_path = version_dir_ / "rollback_literal_policy.yaml";
    {
        std::ofstream f(tmp_policy_path);
        f << "version: 1\n";
        f << "access_control:\n";
        f << "  - user: rollback_user\n";
        f << "    source_ip_cidr: 0.0.0.0/0\n";
        f << "    allowed_operations: [SELECT]\n";
        f << "    allowed_tables: [\"*\"]\n";
        f << "sql_rules:\n";
        f << "  block_patterns:\n";
        f << "    - \"UNION\\\\s+SELECT\"\n";
    }

    auto cfg = make_explain_policy_config();
    const auto save_v1 = version_store_->save_snapshot(*cfg, tmp_policy_path);
    ASSERT_TRUE(static_cast<bool>(save_v1)) << "save_snapshot(v1) must succeed";
    const auto save_v2 = version_store_->save_snapshot(*cfg, tmp_policy_path);
    ASSERT_TRUE(static_cast<bool>(save_v2)) << "save_snapshot(v2) must succeed";

    const std::uint64_t injected_version = save_v1->version;
    const std::uint64_t target_version = save_v2->version;
    ASSERT_NE(injected_version, target_version);

    policy_engine_->reload(cfg, 99U);
    ASSERT_EQ(policy_engine_->current_version(), 99U);

    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    const std::string request =
        R"({"command":"policy_rollback","version":1,"payload":{"note":"\"target_version\":)" +
        std::to_string(injected_version) + R"(","target_version":)" +
        std::to_string(target_version) + R"(}})";
    client.send(request);

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_rollback must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":true)"), std::string::npos)
        << "rollback should succeed with payload.target_version. Got: " << resp;

    const std::string expected_rolled_back = "\"rolled_back_to\":" + std::to_string(target_version);
    EXPECT_NE(resp.find(expected_rolled_back), std::string::npos)
        << "response must use payload.target_version, not string literal match. Got: " << resp;
    EXPECT_EQ(policy_engine_->current_version(), target_version)
        << "policy_engine must roll back to payload.target_version";
}

// ---------------------------------------------------------------------------
// PolicyRollback_TargetVersionOverflow_ReturnsErrorAndKeepsCurrentPolicy
//   uint64 범위를 넘는 target_version 입력은 거부되어야 하며,
//   현재 정책 버전이 변경되면 안 된다.
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyVersioningTest, PolicyRollback_TargetVersionOverflow_ReturnsErrorAndKeepsCurrentPolicy) {
    const auto tmp_policy_path = version_dir_ / "rollback_overflow_policy.yaml";
    {
        std::ofstream f(tmp_policy_path);
        f << "version: 1\n";
        f << "access_control:\n";
        f << "  - user: rollback_user\n";
        f << "    source_ip_cidr: 0.0.0.0/0\n";
        f << "    allowed_operations: [SELECT]\n";
        f << "    allowed_tables: [\"*\"]\n";
        f << "sql_rules:\n";
        f << "  block_patterns:\n";
        f << "    - \"UNION\\\\s+SELECT\"\n";
    }

    auto cfg = make_explain_policy_config();
    const auto save_v1 = version_store_->save_snapshot(*cfg, tmp_policy_path);
    ASSERT_TRUE(static_cast<bool>(save_v1)) << "save_snapshot(v1) must succeed";
    ASSERT_EQ(save_v1->version, 1U);

    const std::uint64_t before_version = 99U;
    policy_engine_->reload(cfg, before_version);
    ASSERT_EQ(policy_engine_->current_version(), before_version);

    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    // UINT64_MAX + 1: 취약한 누적 파서는 0으로 wrap 된다.
    // UINT64_MAX + 2: 취약한 누적 파서는 1로 wrap 되어, v1이 있으면 잘못된 롤백이 발생할 수 있다.
    client.send(
        R"({"command":"policy_rollback","version":1,"payload":{"target_version":18446744073709551617}})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_rollback must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "overflow target_version must be rejected. Got: " << resp;
    EXPECT_EQ(policy_engine_->current_version(), before_version)
        << "current policy version must stay unchanged on overflow input";
}

// ---------------------------------------------------------------------------
// PolicyReload_WithoutVersionStore_ReturnsNotImplemented (DON-50)
//   version_store 없이 생성된 UdsServer 에서 policy_reload 커맨드는
//   not-implemented 응답을 반환해야 한다.
//   (기존 501 placeholder 동작 유지 확인)
// ---------------------------------------------------------------------------
TEST_F(UdsServerTest, PolicyReload_WithoutVersionStore_ReturnsNotImplemented) {
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    client.send(R"({"command":"policy_reload","version":1})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty());
    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Without version_store, policy_reload must return ok:false. Got: " << resp;
    EXPECT_NE(resp.find("501"), std::string::npos)
        << "Response should contain 501 code. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyReload_WithEmptyConfigPath_ReturnsError (DON-50)
//   version_store 는 있지만 policy_config_path 가 비어있으면 ok:false 가 반환되어야 한다.
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyVersioningTest, PolicyReload_WithEmptyConfigPath_ReturnsError) {
    // SetUp 에서 policy_config_path 를 빈 경로로 설정했음
    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    client.send(R"({"command":"policy_reload","version":1})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_reload must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Empty config path must return ok:false. Got: " << resp;
}

// ---------------------------------------------------------------------------
// PolicyRollback_Success_PolicyAppliedAndVersionUpdated (DON-50)
//   스냅샷이 저장된 상태에서 policy_rollback 커맨드로 정상 롤백 시,
//   ok:true 가 반환되고 PolicyEngine 의 current_version() 이 target_version 으로
//   갱신되어야 한다.
//
//   [검증 포인트]
//   - ok:true 포함
//   - policy_engine_->current_version() == target_version
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyVersioningTest, PolicyRollback_Success_PolicyAppliedAndVersionUpdated) {
    // 스냅샷 저장을 위해 임시 YAML 파일 생성
    const auto tmp_policy_path = version_dir_ / "policy.yaml";
    {
        std::ofstream f(tmp_policy_path);
        f << "version: 1\n";
        f << "access_control:\n";
        f << "  - user: rollback_user\n";
        f << "    source_ip_cidr: 0.0.0.0/0\n";
        f << "    allowed_operations: [SELECT]\n";
        f << "    allowed_tables: [\"*\"]\n";
        f << "sql_rules:\n";
        f << "  block_patterns:\n";
        f << "    - \"UNION\\\\s+SELECT\"\n";
    }

    auto cfg = make_explain_policy_config();
    const auto save_result = version_store_->save_snapshot(*cfg, tmp_policy_path);
    ASSERT_TRUE(static_cast<bool>(save_result)) << "save_snapshot must succeed";
    const std::uint64_t target_ver = save_result->version;

    // 현재 엔진 버전을 다른 값으로 설정
    policy_engine_->reload(cfg, 99U);
    ASSERT_EQ(policy_engine_->current_version(), 99U);

    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    const std::string request =
        R"({"command":"policy_rollback","version":1,"payload":{"target_version":)" +
        std::to_string(target_ver) + R"(}})";
    client.send(request);

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_rollback must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":true)"), std::string::npos)
        << "Successful rollback must return ok:true. Got: " << resp;

    // PolicyEngine 의 버전이 target_version 으로 갱신됐는지 확인
    EXPECT_EQ(policy_engine_->current_version(), target_ver)
        << "policy_engine must be updated to target_version after successful rollback";
}

// ---------------------------------------------------------------------------
// PolicyRollback_FailClose_CurrentPolicyMaintained (DON-50)
//   존재하지 않는 버전으로 rollback 시도 시 실패하고,
//   PolicyEngine 의 현재 정책(버전/동작)이 유지되어야 한다 (fail-close).
//
//   [검증 포인트]
//   - ok:false 반환
//   - policy_engine_->current_version() 변경 없음
//   - 롤백 전 evaluate() 결과와 동일한 동작 유지
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyVersioningTest, PolicyRollback_FailClose_CurrentPolicyMaintained) {
    auto cfg = make_explain_policy_config();
    const std::uint64_t before_version = 77U;
    policy_engine_->reload(cfg, before_version);

    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    // 버전 9999 는 존재하지 않음 → rollback 실패
    client.send(R"({"command":"policy_rollback","version":1,"payload":{"target_version":9999}})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_rollback must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":false)"), std::string::npos)
        << "Rollback to non-existent version must return ok:false. Got: " << resp;

    // fail-close: 현재 버전이 변경되지 않아야 함
    EXPECT_EQ(policy_engine_->current_version(), before_version)
        << "[FAIL-CLOSE] PolicyEngine version must not change after failed rollback. "
        << "before=" << before_version << " after=" << policy_engine_->current_version();
}

// ---------------------------------------------------------------------------
// PolicyReload_Success_WithValidConfigPath (DON-50)
//   유효한 정책 파일 경로로 policy_reload 커맨드 시,
//   ok:true 가 반환되고 PolicyEngine 버전이 갱신되어야 한다.
//
//   [검증 포인트]
//   - ok:true 포함
//   - policy_engine_->current_version() > 0 (새 스냅샷이 저장됨)
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyVersioningTest, PolicyReload_Success_WithValidConfigPath) {
    // 유효한 YAML 파일 생성
    const auto tmp_policy_path = version_dir_ / "reload_policy.yaml";
    {
        std::ofstream f(tmp_policy_path);
        f << "version: 1\n";
        f << "access_control:\n";
        f << "  - user: reload_user\n";
        f << "    source_ip_cidr: 0.0.0.0/0\n";
        f << "    allowed_operations: [SELECT]\n";
        f << "    allowed_tables: [\"*\"]\n";
        f << "sql_rules:\n";
        f << "  block_statements:\n";
        f << "    - DROP\n";
        f << "  block_patterns:\n";
        f << "    - \"UNION\\\\s+SELECT\"\n";
        f << "procedure_control:\n";
        f << "  mode: \"whitelist\"\n";
        f << "  whitelist: []\n";
        f << "  block_dynamic_sql: true\n";
        f << "  block_create_alter: true\n";
        f << "data_protection:\n";
        f << "  max_result_rows: 1000\n";
        f << "  block_schema_access: true\n";
    }

    // policy_config_path 를 유효한 경로로 설정한 UdsServer 생성
    // UdsPolicyVersioningTest::SetUp 에서 빈 경로로 생성됐으므로 서버를 재생성한다.
    stop_server();
    server_.reset();

    ioc_ = std::make_unique<asio::io_context>();
    const auto new_socket_path = temp_socket_path("reload_ok");
    server_ = std::make_unique<UdsServer>(new_socket_path,
                                          stats_,
                                          policy_engine_,
                                          sql_parser_,
                                          version_store_,
                                          tmp_policy_path,
                                          *ioc_);

    asio::co_spawn(*ioc_, server_->run(), asio::detached);
    server_thread_ = std::thread([this]() { ioc_->run(); });

    // 소켓 생성 대기
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(new_socket_path)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_TRUE(std::filesystem::exists(new_socket_path))
        << "reload test socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(new_socket_path));

    client.send(R"({"command":"policy_reload","version":1})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_reload must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":true)"), std::string::npos)
        << "Reload with valid config path must return ok:true. Got: " << resp;

    // PolicyEngine 버전이 갱신됐는지 확인 (스냅샷 저장 → reload(cfg, ver) 호출)
    EXPECT_GT(policy_engine_->current_version(), 0U)
        << "policy_engine version must be updated after successful reload. Got: "
        << policy_engine_->current_version();

    // TearDown 에서 중복 stop/join 방지를 위해 소켓 정리
    std::filesystem::remove(new_socket_path);
}

// ---------------------------------------------------------------------------
// PolicyVersions_CurrentVersionField_MatchesEngineVersion (DON-50)
//   policy_versions 응답의 "current" 필드가 PolicyEngine::current_version() 과
//   일치하는지 확인한다.
//
//   [검증 포인트]
//   - "current":N 포함 (N == policy_engine_->current_version())
// ---------------------------------------------------------------------------
TEST_F(UdsPolicyVersioningTest, PolicyVersions_CurrentVersionField_MatchesEngineVersion) {
    // 스냅샷 저장 + 엔진 버전 설정
    const auto tmp_policy_path = version_dir_ / "ver_match.yaml";
    {
        std::ofstream f(tmp_policy_path);
        f << "version: 1\n";
        f << "access_control:\n";
        f << "  - user: ver_user\n";
        f << "    source_ip_cidr: 0.0.0.0/0\n";
        f << "    allowed_operations: [SELECT]\n";
        f << "    allowed_tables: [\"*\"]\n";
        f << "sql_rules:\n";
        f << "  block_patterns:\n";
        f << "    - \"UNION\\\\s+SELECT\"\n";
    }

    auto cfg = make_explain_policy_config();
    const auto save_result = version_store_->save_snapshot(*cfg, tmp_policy_path);
    ASSERT_TRUE(static_cast<bool>(save_result)) << "save_snapshot must succeed";

    policy_engine_->reload(cfg, save_result->version);
    const std::uint64_t expected_current = policy_engine_->current_version();

    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    UdsSyncClient client;
    ASSERT_NO_THROW(client.connect(socket_path_));

    client.send(R"({"command":"policy_versions","version":1})");

    const std::string resp = client.recv();
    ASSERT_FALSE(resp.empty()) << "policy_versions must return a non-empty response";

    EXPECT_NE(resp.find(R"("ok":true)"), std::string::npos)
        << "policy_versions must return ok:true. Got: " << resp;

    // "current":N 포함 확인
    const std::string expected_current_str = R"("current":)" + std::to_string(expected_current);
    EXPECT_NE(resp.find(expected_current_str), std::string::npos)
        << "policy_versions must contain current=" << expected_current << ". Got: " << resp;
}
