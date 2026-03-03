#pragma once

// ---------------------------------------------------------------------------
// async_stream.hpp  —  AsyncStream: TCP/SSL 타입 소거 래퍼
//
// 설계 목적:
//   클라이언트↔프록시 구간 및 프록시↔MySQL 구간 양쪽 모두 TLS를 지원하기
//   위해, tcp::socket과 ssl::stream<tcp::socket>을 std::variant로 보유하고
//   통일된 async_read_some / async_write_some 인터페이스를 제공한다.
//
// 사용 패턴:
//   // 평문 모드
//   AsyncStream stream{std::move(tcp_socket)};
//
//   // TLS 모드
//   AsyncStream stream{ssl::stream<tcp::socket>{std::move(tcp_socket), ssl_ctx}};
//
//   // 공통 읽기/쓰기 (co_await 사용)
//   co_await async_read(stream, buffer, use_awaitable);
//
// 주의:
//   - 이동 전용 (복사 불가)
//   - async_handshake: 평문 모드에서는 즉시 성공(no-op)
//   - async_shutdown:  평문 모드에서는 no-op
//   - lowest_layer():  TCP 소켓 직접 접근 (connect/close/cancel용)
// ---------------------------------------------------------------------------

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/system/error_code.hpp>
#include <type_traits>
#include <variant>

// ---------------------------------------------------------------------------
// AsyncStream
// ---------------------------------------------------------------------------
class AsyncStream {
public:
    using executor_type = boost::asio::any_io_executor;
    using tcp_socket = boost::asio::ip::tcp::socket;
    using ssl_socket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

    // -----------------------------------------------------------------------
    // 생성자 — tcp::socket (평문 모드)
    // -----------------------------------------------------------------------
    explicit AsyncStream(tcp_socket socket);

    // -----------------------------------------------------------------------
    // 생성자 — ssl::stream<tcp::socket> (TLS 모드)
    // -----------------------------------------------------------------------
    explicit AsyncStream(ssl_socket ssl_stream);

    // 이동 생성자 / 이동 대입
    AsyncStream(AsyncStream&&) noexcept;
    AsyncStream& operator=(AsyncStream&&) noexcept;

    // 복사 금지
    AsyncStream(const AsyncStream&) = delete;
    AsyncStream& operator=(const AsyncStream&) = delete;

    ~AsyncStream();

    // -----------------------------------------------------------------------
    // get_executor
    // -----------------------------------------------------------------------
    auto get_executor() -> executor_type;

    // -----------------------------------------------------------------------
    // async_read_some  (템플릿 — 헤더에 구현)
    //   std::visit으로 실제 스트림에 위임.
    // -----------------------------------------------------------------------
    template <typename MutableBufferSequence, typename ReadToken>
    auto async_read_some(const MutableBufferSequence& buffers, ReadToken&& token) {
        return std::visit(
            [&](auto& s) { return s.async_read_some(buffers, std::forward<ReadToken>(token)); },
            stream_);
    }

    // -----------------------------------------------------------------------
    // async_write_some  (템플릿 — 헤더에 구현)
    // -----------------------------------------------------------------------
    template <typename ConstBufferSequence, typename WriteToken>
    auto async_write_some(const ConstBufferSequence& buffers, WriteToken&& token) {
        return std::visit(
            [&](auto& s) { return s.async_write_some(buffers, std::forward<WriteToken>(token)); },
            stream_);
    }

    // -----------------------------------------------------------------------
    // async_handshake  (템플릿 — 헤더에 구현)
    //   평문 모드: no-op (boost::asio::post로 비동기 완료)
    //   TLS  모드: ssl::stream::async_handshake 위임
    // -----------------------------------------------------------------------
    template <typename Token>
    auto async_handshake(boost::asio::ssl::stream_base::handshake_type type, Token&& token) {
        return std::visit(
            [&](auto& s) {
                using S = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<S, ssl_socket>) {
                    return s.async_handshake(type, std::forward<Token>(token));
                } else {
                    // 평문 모드: 즉시 성공 반환 (비동기)
                    return boost::asio::async_initiate<Token, void(boost::system::error_code)>(
                        [&s](auto handler) {
                            boost::asio::post(s.get_executor(), [h = std::move(handler)]() mutable {
                                h(boost::system::error_code{});
                            });
                        },
                        token);
                }
            },
            stream_);
    }

    // -----------------------------------------------------------------------
    // async_shutdown  (템플릿 — 헤더에 구현)
    //   평문 모드: no-op (boost::asio::post로 비동기 완료)
    //   TLS  모드: ssl::stream::async_shutdown 위임
    // -----------------------------------------------------------------------
    template <typename Token>
    auto async_shutdown(Token&& token) {
        return std::visit(
            [&](auto& s) {
                using S = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<S, ssl_socket>) {
                    return s.async_shutdown(std::forward<Token>(token));
                } else {
                    // 평문 모드: 즉시 성공 반환 (비동기)
                    return boost::asio::async_initiate<Token, void(boost::system::error_code)>(
                        [&s](auto handler) {
                            boost::asio::post(s.get_executor(), [h = std::move(handler)]() mutable {
                                h(boost::system::error_code{});
                            });
                        },
                        token);
                }
            },
            stream_);
    }

    // -----------------------------------------------------------------------
    // lowest_layer
    //   TCP 소켓 직접 참조 (connect / close / cancel / remote_endpoint 용)
    // -----------------------------------------------------------------------
    auto lowest_layer() -> tcp_socket&;

    // -----------------------------------------------------------------------
    // is_ssl
    //   현재 TLS 모드인지 여부.
    // -----------------------------------------------------------------------
    [[nodiscard]] bool is_ssl() const noexcept;

private:
    std::variant<tcp_socket, ssl_socket> stream_;
};
