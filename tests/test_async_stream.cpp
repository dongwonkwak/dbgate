// ---------------------------------------------------------------------------
// test_async_stream.cpp
//
// AsyncStream 단위 테스트.
//
// 네트워크 I/O가 필요한 TLS 핸드셰이크 검증은 통합 테스트에서 수행한다.
// 여기서는 생성/이동/타입 판별/lowest_layer/get_executor 등 비I/O 항목을 검증한다.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>

#include "common/async_stream.hpp"

// ---------------------------------------------------------------------------
// 헬퍼
// ---------------------------------------------------------------------------
namespace {

boost::asio::ip::tcp::socket make_tcp_socket(boost::asio::io_context& ioc) {
    return boost::asio::ip::tcp::socket{ioc};
}

}  // namespace

// ---------------------------------------------------------------------------
// 생성자 테스트
// ---------------------------------------------------------------------------

TEST(AsyncStreamTest, ConstructFromTcpSocket) {
    boost::asio::io_context ioc;
    AsyncStream stream{make_tcp_socket(ioc)};

    EXPECT_FALSE(stream.is_ssl());
}

TEST(AsyncStreamTest, ConstructFromSslSocket) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tls_client};

    AsyncStream::ssl_socket ssl_sock{make_tcp_socket(ioc), ssl_ctx};
    AsyncStream stream{std::move(ssl_sock)};

    EXPECT_TRUE(stream.is_ssl());
}

// ---------------------------------------------------------------------------
// 이동 생성자 테스트
// ---------------------------------------------------------------------------

TEST(AsyncStreamTest, MoveConstructorTcpSocket) {
    boost::asio::io_context ioc;
    AsyncStream stream1{make_tcp_socket(ioc)};

    EXPECT_FALSE(stream1.is_ssl());

    AsyncStream stream2{std::move(stream1)};
    EXPECT_FALSE(stream2.is_ssl());
}

TEST(AsyncStreamTest, MoveConstructorSslSocket) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tls_client};

    AsyncStream::ssl_socket ssl_sock{make_tcp_socket(ioc), ssl_ctx};
    AsyncStream stream1{std::move(ssl_sock)};

    EXPECT_TRUE(stream1.is_ssl());

    AsyncStream stream2{std::move(stream1)};
    EXPECT_TRUE(stream2.is_ssl());
}

// ---------------------------------------------------------------------------
// 이동 대입 연산자 테스트
// ---------------------------------------------------------------------------

TEST(AsyncStreamTest, MoveAssignTcpToTcp) {
    boost::asio::io_context ioc;
    AsyncStream stream1{make_tcp_socket(ioc)};
    AsyncStream stream2{make_tcp_socket(ioc)};

    stream2 = std::move(stream1);
    EXPECT_FALSE(stream2.is_ssl());
}

TEST(AsyncStreamTest, MoveAssignSslToTcp) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tls_client};

    // 평문 → TLS로 교체 (backend SSL 업그레이드 시나리오 재현)
    AsyncStream stream{make_tcp_socket(ioc)};
    EXPECT_FALSE(stream.is_ssl());

    AsyncStream::ssl_socket ssl_sock{make_tcp_socket(ioc), ssl_ctx};
    stream = AsyncStream{std::move(ssl_sock)};
    EXPECT_TRUE(stream.is_ssl());
}

TEST(AsyncStreamTest, MoveAssignTcpToSsl) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tls_client};

    AsyncStream::ssl_socket ssl_sock{make_tcp_socket(ioc), ssl_ctx};
    AsyncStream stream{std::move(ssl_sock)};
    EXPECT_TRUE(stream.is_ssl());

    // TLS → 평문으로 교체
    stream = AsyncStream{make_tcp_socket(ioc)};
    EXPECT_FALSE(stream.is_ssl());
}

// ---------------------------------------------------------------------------
// is_ssl() 테스트
// ---------------------------------------------------------------------------

TEST(AsyncStreamTest, IsSslFalseForTcp) {
    boost::asio::io_context ioc;
    AsyncStream stream{make_tcp_socket(ioc)};
    EXPECT_FALSE(stream.is_ssl());
}

TEST(AsyncStreamTest, IsSslTrueForSsl) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tls_client};

    AsyncStream::ssl_socket ssl_sock{make_tcp_socket(ioc), ssl_ctx};
    AsyncStream stream{std::move(ssl_sock)};
    EXPECT_TRUE(stream.is_ssl());
}

// ---------------------------------------------------------------------------
// lowest_layer() 테스트
// ---------------------------------------------------------------------------

TEST(AsyncStreamTest, LowestLayerReturnsTcpSocketForPlain) {
    boost::asio::io_context ioc;
    AsyncStream stream{make_tcp_socket(ioc)};

    // lowest_layer()가 tcp::socket& 를 반환해야 함 (컴파일 검증)
    boost::asio::ip::tcp::socket& tcp_sock = stream.lowest_layer();

    // 기본 상태에서 소켓은 열려 있지 않아야 함
    EXPECT_FALSE(tcp_sock.is_open());
}

TEST(AsyncStreamTest, LowestLayerReturnsTcpSocketForSsl) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tls_client};

    AsyncStream::ssl_socket ssl_sock{make_tcp_socket(ioc), ssl_ctx};
    AsyncStream stream{std::move(ssl_sock)};

    boost::asio::ip::tcp::socket& tcp_sock = stream.lowest_layer();
    EXPECT_FALSE(tcp_sock.is_open());
}

// ---------------------------------------------------------------------------
// get_executor() 테스트
// ---------------------------------------------------------------------------

TEST(AsyncStreamTest, GetExecutorFromTcpSocket) {
    boost::asio::io_context ioc;
    AsyncStream stream{make_tcp_socket(ioc)};

    // executor가 반환되면 성공 (타입 확인)
    auto ex = stream.get_executor();
    EXPECT_TRUE(static_cast<bool>(ex));
}

TEST(AsyncStreamTest, GetExecutorFromSslSocket) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tls_client};

    AsyncStream::ssl_socket ssl_sock{make_tcp_socket(ioc), ssl_ctx};
    AsyncStream stream{std::move(ssl_sock)};

    auto ex = stream.get_executor();
    EXPECT_TRUE(static_cast<bool>(ex));
}

// ---------------------------------------------------------------------------
// close() via lowest_layer()  — 실제 소켓 없이 닫기 시도 (에러 무시)
// ---------------------------------------------------------------------------

TEST(AsyncStreamTest, CloseViaLowestLayerNoThrow) {
    boost::asio::io_context ioc;
    AsyncStream stream{make_tcp_socket(ioc)};

    // 연결되지 않은 소켓 닫기는 에러를 무시하면 crashing하지 않아야 함
    EXPECT_NO_THROW({
        boost::system::error_code ec;
        stream.lowest_layer().close(ec);
        // ec가 설정돼도 크래시 없음
    });
}

// ---------------------------------------------------------------------------
// async_handshake no-op (평문 모드) 테스트
//   io_context를 run()해서 콜백이 호출되는지 확인한다.
// ---------------------------------------------------------------------------

TEST(AsyncStreamTest, AsyncHandshakeNoOpForTcp) {
    boost::asio::io_context ioc;
    AsyncStream stream{make_tcp_socket(ioc)};

    bool called = false;
    boost::system::error_code result_ec{boost::asio::error::would_block};

    stream.async_handshake(boost::asio::ssl::stream_base::client,
                           [&](boost::system::error_code ec) {
                               called = true;
                               result_ec = ec;
                           });

    // no-op은 post()를 통해 비동기로 완료되므로 io_context를 실행해야 함
    ioc.run();

    EXPECT_TRUE(called);
    EXPECT_FALSE(result_ec);  // 성공 (ec == 0)
}

// ---------------------------------------------------------------------------
// async_shutdown no-op (평문 모드) 테스트
// ---------------------------------------------------------------------------

TEST(AsyncStreamTest, AsyncShutdownNoOpForTcp) {
    boost::asio::io_context ioc;
    AsyncStream stream{make_tcp_socket(ioc)};

    bool called = false;
    boost::system::error_code result_ec{boost::asio::error::would_block};

    stream.async_shutdown([&](boost::system::error_code ec) {
        called = true;
        result_ec = ec;
    });

    ioc.run();

    EXPECT_TRUE(called);
    EXPECT_FALSE(result_ec);
}
