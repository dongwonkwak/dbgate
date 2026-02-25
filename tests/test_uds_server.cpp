// ---------------------------------------------------------------------------
// test_uds_server.cpp
//
// UdsServer 단위 테스트.
//
// [테스트 범위]
// - "stats" 커맨드 → 유효한 JSON {"ok":true,"payload":{...}} 응답
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

#include "stats/stats_collector.hpp"
#include "stats/uds_server.hpp"

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using stream_protocol = asio::local::stream_protocol;

// ---------------------------------------------------------------------------
// 익명 네임스페이스: 테스트 내부 헬퍼
// ---------------------------------------------------------------------------
namespace {

// 임시 소켓 경로 생성 (PID + 단조 카운터로 테스트 간 충돌 방지)
std::filesystem::path temp_socket_path(const char* tag) {
    static std::atomic<int> counter{0};
    return std::filesystem::path("/tmp") /
           ("test_uds_" + std::to_string(::getpid()) +
            "_" + std::to_string(counter.fetch_add(1)) +
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
    return static_cast<uint32_t>(b[0])
         | (static_cast<uint32_t>(b[1]) << 8)
         | (static_cast<uint32_t>(b[2]) << 16)
         | (static_cast<uint32_t>(b[3]) << 24);
}

// ---------------------------------------------------------------------------
// UdsSyncClient
//   동기 UDS 클라이언트 (테스트 전용).
//   자체 io_context 를 보유하여 서버 ioc 와 완전히 분리된다.
// ---------------------------------------------------------------------------
struct UdsSyncClient {
    asio::io_context        ioc;
    stream_protocol::socket sock{ioc};

    // connect: 소켓 경로에 동기 연결 (실패 시 예외)
    void connect(const std::filesystem::path& path) {
        sock.connect(stream_protocol::endpoint{path.string()});
    }

    // send: [4LE 헤더][JSON 바디] 형식으로 전송
    void send(std::string_view body) {
        const auto hdr = encode_le4(static_cast<uint32_t>(body.size()));
        std::array<asio::const_buffer, 2> bufs{
            asio::buffer(hdr),
            asio::buffer(body.data(), body.size()),
        };
        asio::write(sock, bufs);
    }

    // recv: [4LE 헤더][바디] 형식으로 수신하여 바디 문자열 반환.
    // 서버가 소켓을 닫으면(EOF/오류) 빈 문자열 반환.
    std::string recv() {
        std::array<uint8_t, 4> hdr{};
        boost::system::error_code ec;
        asio::read(sock, asio::buffer(hdr), ec);
        if (ec) { return {}; }

        const uint32_t len = decode_le4(hdr);
        // 응답이 0 이거나 비현실적으로 크면 오류로 취급
        if (len == 0 || len > 16u * 1024u * 1024u) { return {}; }

        std::string body(len, '\0');
        asio::read(sock, asio::buffer(body), ec);
        if (ec) { return {}; }
        return body;
    }

    // send_raw_header: 헤더만 전송 (malformed frame 테스트용)
    void send_raw_header(uint32_t fake_len) {
        const auto hdr = encode_le4(fake_len);
        boost::system::error_code ec;
        asio::write(sock, asio::buffer(hdr), ec);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// UdsServerTest 픽스처
//   각 테스트마다 독립된 소켓 경로 + UdsServer + io_context 를 생성/정리한다.
// ---------------------------------------------------------------------------
class UdsServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        socket_path_ = temp_socket_path("srv");
        stats_        = std::make_shared<StatsCollector>();
        ioc_          = std::make_unique<asio::io_context>();
        server_       = std::make_unique<UdsServer>(socket_path_, stats_, *ioc_);
    }

    void TearDown() override {
        stop_server();
        std::filesystem::remove(socket_path_);
    }

    // start_server: 백그라운드 스레드에서 서버 io_context 실행
    void start_server() {
        asio::co_spawn(*ioc_, server_->run(), asio::detached);
        server_thread_ = std::thread([this]() { ioc_->run(); });
    }

    // stop_server: 서버를 중단하고 스레드 join
    void stop_server() {
        if (server_) { server_->stop(); }
        ioc_->stop();
        if (server_thread_.joinable()) { server_thread_.join(); }
    }

    // wait_for_socket: 서버가 소켓 파일을 생성할 때까지 polling 대기
    bool wait_for_socket(std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::exists(socket_path_)) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }

    std::filesystem::path             socket_path_;
    std::shared_ptr<StatsCollector>   stats_;
    std::unique_ptr<asio::io_context> ioc_;
    std::unique_ptr<UdsServer>        server_;
    std::thread                       server_thread_;
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
    ASSERT_FALSE(resp.empty())
        << "stats command must return a non-empty response";

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
    ASSERT_FALSE(resp.empty())
        << "Unknown command must return a response (not silence)";

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
    ASSERT_FALSE(resp.empty())
        << "Missing command field must trigger an error response";

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
    client.send_raw_header(0u);

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
    client.send_raw_header(0xFFFFFFFFu);

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
    constexpr int kClientCount = 4;

    start_server();
    ASSERT_TRUE(wait_for_socket()) << "UDS socket not created within 2s";

    std::vector<std::string> responses(static_cast<std::size_t>(kClientCount));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(kClientCount));

    for (int i = 0; i < kClientCount; ++i) {
        threads.emplace_back([this, i, &responses]() {
            UdsSyncClient client;
            try {
                client.connect(socket_path_);
                client.send(R"({"command":"stats","version":1})");
                responses[static_cast<std::size_t>(i)] = client.recv();
            } catch (const std::exception& ex) {
                responses[static_cast<std::size_t>(i)] =
                    std::string("EXCEPTION: ") + ex.what();
            }
        });
    }

    for (auto& t : threads) { t.join(); }

    // 모든 클라이언트가 정상 응답을 받아야 함
    for (int i = 0; i < kClientCount; ++i) {
        const auto& resp = responses[static_cast<std::size_t>(i)];
        EXPECT_FALSE(resp.empty())
            << "Client " << i << " must receive a non-empty response";
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
    ASSERT_NO_THROW(server_->stop())
        << "stop() before run() must not throw or crash";
}
