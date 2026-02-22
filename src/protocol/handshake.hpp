#pragma once

#include "common/types.hpp"
#include "protocol/mysql_packet.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <expected>

// ---------------------------------------------------------------------------
// HandshakeRelay
//   MySQL 핸드셰이크를 클라이언트 ↔ 서버 간 투명하게 릴레이한다.
//
//   설계 원칙:
//     - auth plugin 내용에 개입하지 않는다.
//     - 핸드셰이크 완료 후 SessionContext 의 db_user / db_name 을 채운다.
//     - 핸드셰이크 단계의 모든 패킷은 변조 없이 그대로 전달한다.
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
    //   client_sock : 클라이언트 측 TCP 소켓 (accept 된 소켓)
    //   server_sock : MySQL 서버 측 TCP 소켓 (connect 된 소켓)
    //   ctx         : [out] db_user, db_name, handshake_done 이 채워진다.
    //
    //   반환: 성공 시 std::expected<void, ParseError>{}
    //         실패 시 std::unexpected(ParseError)
    // -----------------------------------------------------------------------
    static auto relay_handshake(
        boost::asio::ip::tcp::socket& client_sock,
        boost::asio::ip::tcp::socket& server_sock,
        SessionContext&               ctx
    ) -> boost::asio::awaitable<std::expected<void, ParseError>>;
};
