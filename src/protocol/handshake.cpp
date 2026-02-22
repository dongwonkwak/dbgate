#include "protocol/handshake.hpp"

#include <boost/asio/use_awaitable.hpp>

// ---------------------------------------------------------------------------
// HandshakeRelay — stub implementation
// ---------------------------------------------------------------------------

// static
auto HandshakeRelay::relay_handshake(
    boost::asio::ip::tcp::socket& /*client_sock*/,
    boost::asio::ip::tcp::socket& /*server_sock*/,
    SessionContext&               /*ctx*/
) -> boost::asio::awaitable<std::expected<void, ParseError>>
{
    co_return std::unexpected(ParseError{
        ParseErrorCode::kInternalError,
        "HandshakeRelay::relay_handshake: not implemented",
        {}
    });
}
