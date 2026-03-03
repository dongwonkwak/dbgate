#pragma once

#include <boost/asio/awaitable.hpp>
#include <expected>

#include "common/async_stream.hpp"
#include "common/types.hpp"
#include "protocol/mysql_packet.hpp"

// ---------------------------------------------------------------------------
// HandshakeRelay
//   MySQL 핸드셰이크를 클라이언트 ↔ 서버 간 투명하게 릴레이한다.
//
//   설계 원칙:
//     - auth plugin 내용에 개입하지 않는다.
//     - 핸드셰이크 완료 후 SessionContext 의 db_user / db_name 을 채운다.
//     - 핸드셰이크 단계의 모든 패킷은 변조 없이 그대로 전달한다.
//     - CLIENT_SSL, CLIENT_DEPRECATE_EOF, CLIENT_QUERY_ATTRIBUTES 비트를
//       서버 greeting 및 클라이언트 응답에서 제거하여 프록시 호환성 유지.
//
//   relay_handshake() 성공 시:
//     - ctx.db_user  : HandshakeResponse 에서 추출한 사용자 이름
//     - ctx.db_name  : HandshakeResponse 에서 추출한 초기 DB 이름
//     - ctx.handshake_done = true
//
//   실패 시:
//     - std::unexpected(ParseError) 반환
//     - 소켓은 호출자가 닫아야 한다.
// ---------------------------------------------------------------------------
class HandshakeRelay {
public:
    HandshakeRelay() = default;

    // -----------------------------------------------------------------------
    // relay_handshake
    //   client_stream : 클라이언트 측 AsyncStream (accept 된 소켓 또는 TLS)
    //   server_stream : MySQL 서버 측 AsyncStream (connect 된 소켓 또는 TLS)
    //   ctx           : [out] db_user, db_name, handshake_done 이 채워진다.
    //
    //   반환: 성공 시 std::expected<void, ParseError>{}
    //         실패 시 std::unexpected(ParseError)
    // -----------------------------------------------------------------------
    static auto relay_handshake(AsyncStream& client_stream,
                                AsyncStream& server_stream,
                                SessionContext& ctx)
        -> boost::asio::awaitable<std::expected<void, ParseError>>;
};
