#include "common/async_stream.hpp"

// ---------------------------------------------------------------------------
// AsyncStream — 비템플릿 메서드 구현
//
// 템플릿 메서드(async_read_some, async_write_some, async_handshake,
// async_shutdown)는 헤더에 inline으로 구현되어 있다.
// 여기서는 생성자, 소멸자, get_executor(), lowest_layer(), is_ssl()만 구현.
// ---------------------------------------------------------------------------

// ─── 생성자 ─────────────────────────────────────────────────────────────────

AsyncStream::AsyncStream(tcp_socket socket) : stream_{std::move(socket)} {}

AsyncStream::AsyncStream(ssl_socket ssl_stream) : stream_{std::move(ssl_stream)} {}

// ─── 이동 생성자 / 이동 대입 ────────────────────────────────────────────────
//
// GCC는 Boost.Asio 소켓이 포함된 std::variant의 move 연산에서
// "service_ may be used uninitialized" 경고를 잘못 발생시킨다.
// = default 대신 명시적으로 std::move(other.stream_)를 사용하면
// 컴파일러가 올바른 경로를 추론하지 못할 수 있으므로,
// 진단 비활성화 pragma를 사용하여 이 경고를 안전하게 억제한다.
//
// 참고: boost::asio::ip::tcp::socket은 이동 후 소멸 시 nop가 보장됨.
//       실제 UAF/uninitialized-use 위험 없음.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

AsyncStream::AsyncStream(AsyncStream&& other) noexcept : stream_{std::move(other.stream_)} {}

AsyncStream& AsyncStream::operator=(AsyncStream&& other) noexcept {
    if (this != &other) {
        stream_ = std::move(other.stream_);
    }
    return *this;
}

#pragma GCC diagnostic pop

// ─── 소멸자 ─────────────────────────────────────────────────────────────────

AsyncStream::~AsyncStream() = default;

// ─── get_executor ───────────────────────────────────────────────────────────

auto AsyncStream::get_executor() -> executor_type {
    return std::visit([](auto& s) -> executor_type { return s.get_executor(); }, stream_);
}

// ─── lowest_layer ───────────────────────────────────────────────────────────
//   variant의 어느 타입이든 tcp::socket 참조를 반환한다.
//   ssl::stream<tcp::socket>의 경우 next_layer()를 통해 접근.
//   (ssl::stream::lowest_layer()는 basic_socket<>을 반환하지만
//    tcp::socket = basic_stream_socket<>이므로 타입 불일치 — next_layer() 사용)

auto AsyncStream::lowest_layer() -> tcp_socket& {
    return std::visit(
        [](auto& s) -> tcp_socket& {
            using S = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<S, ssl_socket>) {
                return s.next_layer();
            } else {
                return s;
            }
        },
        stream_);
}

// ─── is_ssl ─────────────────────────────────────────────────────────────────

bool AsyncStream::is_ssl() const noexcept {
    return std::holds_alternative<ssl_socket>(stream_);
}
