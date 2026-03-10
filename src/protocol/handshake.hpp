#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ssl/context.hpp>
#include <expected>
#include <string>

#include "common/async_stream.hpp"
#include "common/types.hpp"
#include "protocol/mysql_packet.hpp"

// ---------------------------------------------------------------------------
// HandshakeSSLConfig
//   relay_handshake() 에 전달되는 SSL 설정.
//
//   frontend_ssl_ctx:
//     nullptr → frontend SSL 없음 (평문 클라이언트)
//     유효 포인터 → CLIENT_SSL 비트를 서버 greeting에서 유지하고,
//                   클라이언트가 SSLRequest를 보내면 client_stream을
//                   SSL로 업그레이드 후 TLS 핸드셰이크 수행.
//
//   backend_ssl_ctx:
//     nullptr → backend SSL 없음 (평문 MySQL 연결)
//     유효 포인터 → 서버 greeting 수신 후 SSLRequest를 server에 전송,
//                   server_stream을 SSL로 업그레이드 후 TLS 핸드셰이크 수행.
//
//   backend_ssl_verify:
//     true → 서버 인증서 체인 + 호스트명/IP 검증 수행
//
//   backend_tls_server_name:
//     SNI 호스트명 (비어있으면 서버 IP 사용)
// ---------------------------------------------------------------------------
struct HandshakeSSLConfig {
    boost::asio::ssl::context* frontend_ssl_ctx{nullptr};  // nullptr = no frontend SSL
    boost::asio::ssl::context* backend_ssl_ctx{nullptr};   // nullptr = no backend SSL
    bool backend_ssl_verify{false};
    std::string backend_tls_server_name{};
};

// ---------------------------------------------------------------------------
// HandshakeRelay
//   MySQL 핸드셰이크를 클라이언트 ↔ 서버 간 투명하게 릴레이한다.
//
//   설계 원칙:
//     - auth plugin 내용에 개입하지 않는다.
//     - 핸드셰이크 완료 후 SessionContext 의 db_user / db_name 을 채운다.
//     - 핸드셰이크 단계의 모든 패킷은 변조 없이 그대로 전달한다.
//     - CLIENT_SSL 비트는 frontend_ssl_ctx가 있을 때만 서버 greeting에서 유지.
//     - CLIENT_DEPRECATE_EOF, CLIENT_QUERY_ATTRIBUTES 는 항상 제거.
//     - MySQL 프로토콜 레벨 SSL 업그레이드를 수행한다 (설정 시).
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
    //   client_stream : 클라이언트 측 AsyncStream (평문 TCP — MySQL 프로토콜 SSL 업그레이드 수행)
    //   server_stream : MySQL 서버 측 AsyncStream (평문 TCP — MySQL 프로토콜 SSL 업그레이드 수행)
    //   ctx           : [out] db_user, db_name, handshake_done 이 채워진다.
    //   ssl_config    : SSL 설정 (nullptr = 해당 구간 평문)
    //
    //   반환: 성공 시 std::expected<void, ParseError>{}
    //         실패 시 std::unexpected(ParseError)
    // -----------------------------------------------------------------------
    static auto relay_handshake(AsyncStream& client_stream,
                                AsyncStream& server_stream,
                                SessionContext& ctx,
                                const HandshakeSSLConfig& ssl_config = {})
        -> boost::asio::awaitable<std::expected<void, ParseError>>;

    // -----------------------------------------------------------------------
    // is_ssl_request
    //   payload가 SSLRequest 패킷인지 판별한다.
    //   조건: payload 정확히 32바이트 AND CLIENT_SSL bit(0x0800) set
    // -----------------------------------------------------------------------
    [[nodiscard]] static auto is_ssl_request(std::span<const std::uint8_t> payload) noexcept
        -> bool;

    // -----------------------------------------------------------------------
    // build_ssl_request
    //   서버로 전송할 SSLRequest 패킷 바이트를 생성한다.
    //   32바이트 payload: capability_flags(4) + max_packet_size(4) + charset(1) + reserved(23)
    //   seq_id: 패킷 시퀀스 번호 (일반적으로 1)
    // -----------------------------------------------------------------------
    [[nodiscard]] static auto build_ssl_request(std::uint8_t seq_id,
                                                std::uint32_t capability_flags,
                                                std::uint32_t max_packet_size,
                                                std::uint8_t charset) -> std::vector<std::uint8_t>;
};
