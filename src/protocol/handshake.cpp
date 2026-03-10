#include "protocol/handshake.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <spdlog/spdlog.h>

#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <format>

#include "protocol/handshake_detail.hpp"

// AsyncStream은 handshake.hpp → common/async_stream.hpp 경유로 이미 포함됨

// ---------------------------------------------------------------------------
// HandshakeRelay — 구현
//
// MySQL 핸드셰이크를 클라이언트 ↔ 서버 간 투명하게 릴레이한다.
// auth plugin 내용에 개입하지 않는다.
//
// 상태 머신:
//   ServerGreeting → ClientResponse → ServerAuth
//     └─ OK       → 완료
//     └─ ERR      → 실패 (ERR 릴레이 후 종료)
//     └─ EOF      → 실패 (EOF 릴레이 후 종료)
//     └─ AuthSwitch → ClientAuthSwitchReply → ServerAuthSwitchResult
//          └─ OK       → 완료
//          └─ ERR      → 실패
//          └─ AuthMoreData → (반복: ClientMoreData → ServerMoreData) → OK|ERR
//     └─ AuthMoreData → ClientMoreData → ServerMoreData
//          └─ OK       → 완료
//          └─ ERR      → 실패
//          └─ AuthMoreData → (반복, 최대 kMaxRoundTrips 회)
// ---------------------------------------------------------------------------

// AuthMoreData / AuthSwitch 무한 루프 방지 최대 라운드트립 횟수
static constexpr int kMaxRoundTrips = 10;

namespace {

auto apply_sequence_delta(std::vector<std::uint8_t>& bytes, int delta) -> void {
    if (bytes.size() < 4 || delta == 0) {
        return;
    }

    const auto seq = static_cast<int>(bytes[3]);
    bytes[3] = static_cast<std::uint8_t>(seq + delta);
}

// -----------------------------------------------------------------------
// read_packet
//   4바이트 헤더를 읽어 payload 길이를 파악한 뒤,
//   전체 패킷 바이트를 읽어 MysqlPacket으로 파싱한다.
// -----------------------------------------------------------------------
auto read_packet(AsyncStream& stream)
    -> boost::asio::awaitable<std::expected<MysqlPacket, ParseError>> {
    // 4바이트 헤더 읽기
    std::array<std::uint8_t, 4> header{};
    boost::system::error_code ec;

    co_await boost::asio::async_read(stream,
                                     boost::asio::buffer(header),
                                     boost::asio::redirect_error(boost::asio::use_awaitable, ec));

    if (ec) {
        co_return std::unexpected(ParseError{.code = ParseErrorCode::kMalformedPacket,
                                             .message = "failed to read packet header",
                                             .context = ec.message()});
    }

    // payload 길이 파싱 (3바이트 LE)
    const std::uint32_t payload_len = static_cast<std::uint32_t>(header[0]) |
                                      (static_cast<std::uint32_t>(header[1]) << 8U) |
                                      (static_cast<std::uint32_t>(header[2]) << 16U);

    // 전체 패킷 버퍼: 헤더(4) + payload
    std::vector<std::uint8_t> buf(4 + payload_len);
    buf[0] = header[0];
    buf[1] = header[1];
    buf[2] = header[2];
    buf[3] = header[3];

    if (payload_len > 0) {
        co_await boost::asio::async_read(
            stream,
            boost::asio::buffer(buf.data() + 4, payload_len),
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec) {
            co_return std::unexpected(ParseError{.code = ParseErrorCode::kMalformedPacket,
                                                 .message = "failed to read packet payload",
                                                 .context = ec.message()});
        }
    }

    co_return MysqlPacket::parse(std::span<const std::uint8_t>{buf});
}

// -----------------------------------------------------------------------
// strip_unsupported_capabilities
//   서버 Initial Handshake payload에서 프록시가 지원하지 않는 capability
//   비트를 제거한다.
//
//   제거 대상:
//   1) CLIENT_SSL (capability_flags_1 bit 11, 0x0800):
//      keep_client_ssl=false인 경우에만 제거.
//      frontend SSL이 활성화된 경우 클라이언트에게 SSL 지원을 광고해야 하므로
//      해당 비트를 유지한다.
//
//   2) CLIENT_QUERY_ATTRIBUTES (capability_flags_2 bit 11, 0x0800 of upper 2B
//      = full value 0x08000000):
//      활성화 시 COM_QUERY payload 앞에 \x00\x01 attribute header가 붙어
//      SQL 추출 로직이 오파싱. 제거하면 클라이언트가 plain SQL만 전송.
//
//   3) CLIENT_DEPRECATE_EOF (capability_flags_2 bit 8, 0x0100 of upper 2B
//      = full value 0x01000000):
//      활성화 시 Result Set 프로토콜이 변경됨 — column definition 뒤에
//      EOF 패킷 없이 바로 row 데이터가 오고, 마지막 EOF 대신 OK(0xFE)
//      패킷이 옴. relay_server_response의 상태 머신이 전통적인 EOF 기반
//      Result Set만 지원하므로, 이 비트를 제거하여 전통 프로토콜을 강제.
//
//   HandshakeV10 payload 구조 (capability_flags 위치):
//     [1B  protocol_version]
//     [NUL server_version]
//     [4B  connection_id]
//     [8B  auth_plugin_data_part_1]
//     [1B  filler]
//     [2B  capability_flags_1]  ← CLIENT_SSL(bit 11): keep_client_ssl=false 시 제거
//     [1B  charset]
//     [2B  status_flags]
//     [2B  capability_flags_2]  ← CLIENT_QUERY_ATTRIBUTES(bit 11),
//                                  CLIENT_DEPRECATE_EOF(bit 8) 제거
//     ...
//
//   파싱 실패 시 원본 직렬화 바이트를 반환한다 (fail-safe).
// -----------------------------------------------------------------------
auto strip_unsupported_capabilities(const MysqlPacket& pkt, bool keep_client_ssl)
    -> std::vector<std::uint8_t> {
    auto bytes = pkt.serialize();
    const auto payload = pkt.payload();

    // protocol_version(1) + server_version(최소 1B NUL) 필요
    if (payload.size() < 2) {
        return bytes;
    }

    // server_version NUL terminator 탐색 (offset 1부터)
    std::size_t pos = 1;
    while (pos < payload.size() && payload[pos] != 0x00) {
        ++pos;
    }
    if (pos >= payload.size()) {
        return bytes;  // NUL terminator 없음 — 원본 반환
    }
    ++pos;  // NUL terminator 건너뜀

    // connection_id(4) + auth_plugin_data_part_1(8) + filler(1) = 13바이트
    pos += 13;

    // capability_flags_1 (2바이트 LE) 위치 확인
    // capability_flags_2는 cap_flags_1(2) + charset(1) + status(2) = 5바이트 이후
    if (pos + 7 > payload.size()) {
        return bytes;
    }

    // serialized bytes에서 payload는 4바이트 헤더 이후
    const std::size_t cap1_offset = 4 + pos;
    const std::size_t cap2_offset = cap1_offset + 5;  // +charset(1)+status(2)+cap_flags_1(2)

    // 1) CLIENT_SSL = 0x0800 in cap_flags_1: high byte bit 3
    //    keep_client_ssl=true이면 유지 (frontend SSL 광고용)
    if (!keep_client_ssl) {
        bytes[cap1_offset + 1] &= static_cast<std::uint8_t>(~0x08U);
    }

    // cap_flags_2 high byte (bits 24-31):
    //   bit 0 (0x01) = CLIENT_DEPRECATE_EOF   (full: 0x01000000)
    //   bit 3 (0x08) = CLIENT_QUERY_ATTRIBUTES (full: 0x08000000)
    if (cap2_offset + 1 < bytes.size()) {
        bytes[cap2_offset + 1] &= static_cast<std::uint8_t>(~0x09U);
    }

    return bytes;
}

// -----------------------------------------------------------------------
// strip_unsupported_client_capabilities
//   클라이언트 HandshakeResponse41 payload의 capability_flags(4B LE)에서
//   프록시가 지원하지 않는 비트를 제거한다.
//
//   keep_client_ssl=true이면 CLIENT_SSL은 제거하지 않는다
//   (frontend SSL 업그레이드 완료 후 진짜 HandshakeResponse41을 서버에 중계할 때).
//
//   payload layout (offset 0):
//     [4B capability_flags] [4B max_packet_size] [1B charset] [23B reserved] ...
// -----------------------------------------------------------------------
auto strip_unsupported_client_capabilities(const MysqlPacket& pkt, bool keep_client_ssl)
    -> std::vector<std::uint8_t> {
    auto bytes = pkt.serialize();
    const auto payload = pkt.payload();

    // HandshakeResponse41 capability_flags(4B) 최소 길이 확인
    if (payload.size() < 4 || bytes.size() < 8) {
        return bytes;
    }

    // keep_client_ssl=true이면 CLIENT_SSL(0x0800) 제거하지 않고 강제 SET
    // (backend SSL 시 클라이언트가 CLIENT_SSL 없이 보내도 서버는 이를 기대하므로)
    const std::uint32_t ssl_mask = keep_client_ssl ? 0U : 0x00000800U;

    const std::uint32_t unsupported_mask = ssl_mask | 0x01000000U  // CLIENT_DEPRECATE_EOF
                                           | 0x08000000U;          // CLIENT_QUERY_ATTRIBUTES

    // serialized bytes에서 payload 시작 오프셋은 4
    std::uint32_t cap_flags = static_cast<std::uint32_t>(bytes[4]) |
                              (static_cast<std::uint32_t>(bytes[5]) << 8U) |
                              (static_cast<std::uint32_t>(bytes[6]) << 16U) |
                              (static_cast<std::uint32_t>(bytes[7]) << 24U);

    cap_flags &= ~unsupported_mask;

    // backend SSL 시 CLIENT_SSL 강제 SET (클라이언트가 평문으로 접속해도
    // 서버는 SSLRequest를 받았으므로 HandshakeResponse41에 CLIENT_SSL 필요)
    if (keep_client_ssl) {
        cap_flags |= 0x00000800U;
    }

    bytes[4] = static_cast<std::uint8_t>(cap_flags & 0xFFU);
    bytes[5] = static_cast<std::uint8_t>((cap_flags >> 8U) & 0xFFU);
    bytes[6] = static_cast<std::uint8_t>((cap_flags >> 16U) & 0xFFU);
    bytes[7] = static_cast<std::uint8_t>((cap_flags >> 24U) & 0xFFU);

    return bytes;
}

// -----------------------------------------------------------------------
// write_packet
//   MysqlPacket을 serialize()한 뒤 소켓에 비동기 전송한다.
// -----------------------------------------------------------------------
auto write_packet(AsyncStream& stream, const MysqlPacket& pkt)
    -> boost::asio::awaitable<std::expected<void, ParseError>> {
    const auto bytes = pkt.serialize();
    boost::system::error_code ec;

    co_await boost::asio::async_write(stream,
                                      boost::asio::buffer(bytes),
                                      boost::asio::redirect_error(boost::asio::use_awaitable, ec));

    if (ec) {
        co_return std::unexpected(ParseError{.code = ParseErrorCode::kInternalError,
                                             .message = "failed to write packet",
                                             .context = ec.message()});
    }

    co_return std::expected<void, ParseError>{};
}

// -----------------------------------------------------------------------
// write_raw_bytes
//   원시 바이트를 소켓에 비동기 전송한다.
// -----------------------------------------------------------------------
auto write_raw_bytes(AsyncStream& stream, const std::vector<std::uint8_t>& bytes)
    -> boost::asio::awaitable<std::expected<void, ParseError>> {
    boost::system::error_code ec;
    co_await boost::asio::async_write(stream,
                                      boost::asio::buffer(bytes),
                                      boost::asio::redirect_error(boost::asio::use_awaitable, ec));

    if (ec) {
        co_return std::unexpected(ParseError{.code = ParseErrorCode::kInternalError,
                                             .message = "failed to write raw bytes",
                                             .context = ec.message()});
    }
    co_return std::expected<void, ParseError>{};
}

// -----------------------------------------------------------------------
// setup_backend_tls
//   server_stream을 SSL로 업그레이드하고 SNI/인증서 검증 설정 후
//   TLS 핸드셰이크를 수행한다.
// -----------------------------------------------------------------------
auto setup_backend_tls(AsyncStream& server_stream,
                       boost::asio::ssl::context& ctx,
                       bool verify_peer,
                       const std::string& server_name)
    -> boost::asio::awaitable<std::expected<void, ParseError>> {
    // tcp→ssl 타입 전환 (동기)
    auto upgrade_result = server_stream.upgrade_to_ssl(ctx);
    if (!upgrade_result) {
        co_return std::unexpected(ParseError{.code = ParseErrorCode::kInternalError,
                                             .message = "backend SSL upgrade failed",
                                             .context = upgrade_result.error()});
    }

    // SNI 및 인증서 검증 설정
    SSL* ssl_handle = server_stream.native_ssl_handle();
    if (ssl_handle != nullptr && !server_name.empty()) {
        // IP 주소인지 확인
        boost::system::error_code ip_ec;
        const bool is_ip =
            !boost::asio::ip::make_address(server_name, ip_ec).is_unspecified() && !ip_ec;

        // SNI는 호스트명 기반 TLS에서만 설정한다.
        if (!is_ip) {
            if (SSL_set_tlsext_host_name(ssl_handle, server_name.c_str()) != 1) {
                const auto err = ERR_get_error();
                co_return std::unexpected(
                    ParseError{.code = ParseErrorCode::kInternalError,
                               .message = "backend TLS SNI setup failed",
                               .context = err != 0 ? ERR_error_string(err, nullptr) : server_name});
            }
        }

        if (verify_peer) {
            int verify_ok = 0;
            if (is_ip) {
                X509_VERIFY_PARAM* param = SSL_get0_param(ssl_handle);
                verify_ok = X509_VERIFY_PARAM_set1_ip_asc(param, server_name.c_str());
            } else {
                verify_ok = SSL_set1_host(ssl_handle, server_name.c_str());
            }
            if (verify_ok != 1) {
                const auto err = ERR_get_error();
                co_return std::unexpected(
                    ParseError{.code = ParseErrorCode::kInternalError,
                               .message = "backend TLS hostname verification setup failed",
                               .context = err != 0 ? ERR_error_string(err, nullptr) : server_name});
            }
        }
    }

    // TLS 핸드셰이크 (프록시 → MySQL: client 역할)
    boost::system::error_code tls_ec;
    co_await server_stream.async_handshake(
        boost::asio::ssl::stream_base::client,
        boost::asio::redirect_error(boost::asio::use_awaitable, tls_ec));

    if (tls_ec) {
        co_return std::unexpected(ParseError{.code = ParseErrorCode::kInternalError,
                                             .message = "backend TLS handshake failed",
                                             .context = tls_ec.message()});
    }
    co_return std::expected<void, ParseError>{};
}

// -----------------------------------------------------------------------
// setup_frontend_tls
//   client_stream을 SSL로 업그레이드하고 TLS 핸드셰이크를 수행한다.
//   (프록시 → 클라이언트: server 역할)
// -----------------------------------------------------------------------
auto setup_frontend_tls(AsyncStream& client_stream, boost::asio::ssl::context& ctx)
    -> boost::asio::awaitable<std::expected<void, ParseError>> {
    auto upgrade_result = client_stream.upgrade_to_ssl(ctx);
    if (!upgrade_result) {
        co_return std::unexpected(ParseError{.code = ParseErrorCode::kInternalError,
                                             .message = "frontend SSL upgrade failed",
                                             .context = upgrade_result.error()});
    }

    boost::system::error_code tls_ec;
    co_await client_stream.async_handshake(
        boost::asio::ssl::stream_base::server,
        boost::asio::redirect_error(boost::asio::use_awaitable, tls_ec));

    if (tls_ec) {
        co_return std::unexpected(ParseError{.code = ParseErrorCode::kInternalError,
                                             .message = "frontend TLS handshake failed",
                                             .context = tls_ec.message()});
    }
    co_return std::expected<void, ParseError>{};
}

// -----------------------------------------------------------------------
// SslUpgradeState — frontend/backend SSL 업그레이드 추적 및 시퀀스 델타 계산
// -----------------------------------------------------------------------
struct SslUpgradeState {
    bool frontend = false;
    bool backend = false;

    // 클라이언트→서버 방향의 seq_id 보정량:
    //   backend SSL이 끼어들면 서버 측 seq_id가 +1 밀리고,
    //   frontend SSL이 끼어들면 클라이언트 측 seq_id가 +1 밀린다.
    //   따라서 프록시가 client→server로 패킷을 릴레이할 때
    //   (backend - frontend) 만큼 seq_id를 조정해야 한다.
    [[nodiscard]] auto client_to_server_delta() const noexcept -> int {
        return (backend ? 1 : 0) - (frontend ? 1 : 0);
    }

    [[nodiscard]] auto server_to_client_delta() const noexcept -> int {
        return (frontend ? 1 : 0) - (backend ? 1 : 0);
    }
};

// -----------------------------------------------------------------------
// relay_with_delta
//   bytes에 seq_id delta를 적용한 뒤 stream에 비동기 전송한다.
//   delta=0이면 apply_sequence_delta가 no-op이므로 그대로 전송.
// -----------------------------------------------------------------------
auto relay_with_delta(AsyncStream& stream,
                      std::vector<std::uint8_t> bytes,
                      int delta,
                      std::string_view err_msg)
    -> boost::asio::awaitable<std::expected<void, ParseError>> {
    apply_sequence_delta(bytes, delta);
    boost::system::error_code write_ec;
    co_await boost::asio::async_write(
        stream,
        boost::asio::buffer(bytes),
        boost::asio::redirect_error(boost::asio::use_awaitable, write_ec));
    if (write_ec) {
        co_return std::unexpected(ParseError{.code = ParseErrorCode::kInternalError,
                                             .message = std::string(err_msg),
                                             .context = write_ec.message()});
    }
    co_return std::expected<void, ParseError>{};
}

}  // namespace

// ---------------------------------------------------------------------------
// detail namespace 구현 — 순수 함수 (소켓 무관)
// ---------------------------------------------------------------------------

namespace detail {

// ---------------------------------------------------------------------------
// classify_auth_response
// ---------------------------------------------------------------------------
auto classify_auth_response(std::span<const std::uint8_t> payload) noexcept -> AuthResponseType {
    if (payload.empty()) {
        return AuthResponseType::kUnknown;
    }

    const std::uint8_t first = payload[0];

    switch (first) {
        case 0x00:
            return AuthResponseType::kOk;
        case 0xFF:
            return AuthResponseType::kError;
        case 0xFE:
            // payload < 9 → EOF (핸드셰이크 실패)
            // payload >= 9 → AuthSwitchRequest (추가 라운드트립)
            if (payload.size() < 9) {
                return AuthResponseType::kEof;
            }
            return AuthResponseType::kAuthSwitch;
        case 0x01:
            return AuthResponseType::kAuthMoreData;
        default:
            return AuthResponseType::kUnknown;
    }
}

// ---------------------------------------------------------------------------
// process_handshake_packet
//   상태 머신 전이 로직. 소켓과 완전히 분리된 순수 함수.
// ---------------------------------------------------------------------------
auto process_handshake_packet(HandshakeState current_state,
                              std::span<const std::uint8_t> payload,
                              int round_trips) noexcept
    -> std::expected<HandshakeTransition, ParseError> {
    switch (current_state) {
        // ---------------------------------------------------------------
        // kWaitServerGreeting: 서버 Initial Handshake 수신
        //   → 무조건 클라이언트에 릴레이, 다음 상태: kWaitClientResponse
        // ---------------------------------------------------------------
        case HandshakeState::kWaitServerGreeting: {
            if (payload.empty()) {
                return std::unexpected(ParseError{.code = ParseErrorCode::kMalformedPacket,
                                                  .message = "empty server greeting payload",
                                                  .context = {}});
            }
            return HandshakeTransition{.next_state = HandshakeState::kWaitClientResponse,
                                       .action = HandshakeAction::kRelayToClient};
        }

        // ---------------------------------------------------------------
        // kWaitClientResponse: 클라이언트 HandshakeResponse 수신
        //   → 무조건 서버에 릴레이, 다음 상태: kWaitServerAuth
        // ---------------------------------------------------------------
        case HandshakeState::kWaitClientResponse: {
            // 최소 길이 검증은 extract_handshake_response_fields에서 수행
            return HandshakeTransition{.next_state = HandshakeState::kWaitServerAuth,
                                       .action = HandshakeAction::kRelayToServer};
        }

        // ---------------------------------------------------------------
        // kWaitServerAuth: 서버 첫 번째 auth 응답 수신
        //   → OK/ERR/EOF/AuthSwitch/AuthMoreData로 분기
        // ---------------------------------------------------------------
        case HandshakeState::kWaitServerAuth: {
            const auto auth_type = classify_auth_response(payload);
            switch (auth_type) {
                case AuthResponseType::kOk:
                    return HandshakeTransition{.next_state = HandshakeState::kDone,
                                               .action = HandshakeAction::kComplete};
                case AuthResponseType::kError:
                case AuthResponseType::kEof:
                    return HandshakeTransition{.next_state = HandshakeState::kFailed,
                                               .action = HandshakeAction::kTerminate};
                case AuthResponseType::kAuthSwitch:
                    return HandshakeTransition{.next_state = HandshakeState::kWaitClientAuthSwitch,
                                               .action = HandshakeAction::kRelayToClient};
                case AuthResponseType::kAuthMoreData: {
                    // caching_sha2_password AuthMoreData 분기:
                    //   payload[1] == 0x03: fast auth OK —
                    //     서버가 캐시로 패스워드 검증 완료. 클라이언트 응답 없이
                    //     서버의 최종 OK 패킷만 기다리므로 kWaitServerMoreData로 전이.
                    //   payload[1] == 0x04 or other: full auth needed —
                    //     클라이언트가 RSA 키 교환 등을 수행해야 하므로 kWaitClientMoreData.
                    const bool fast_auth_ok = (payload.size() >= 2 && payload[1] == 0x03U);
                    return HandshakeTransition{
                        .next_state = fast_auth_ok ? HandshakeState::kWaitServerMoreData
                                                   : HandshakeState::kWaitClientMoreData,
                        .action = HandshakeAction::kRelayToClient};
                }
                case AuthResponseType::kUnknown:
                    return HandshakeTransition{.next_state = HandshakeState::kFailed,
                                               .action = HandshakeAction::kTerminateNoRelay};
            }
            // unreachable — 컴파일러 경고 방지
            return std::unexpected(ParseError{.code = ParseErrorCode::kInternalError,
                                              .message = "unreachable: classify_auth_response",
                                              .context = {}});
        }

        // ---------------------------------------------------------------
        // kWaitClientAuthSwitch: AuthSwitch 후 클라이언트 응답 대기
        //   → 무조건 서버에 릴레이, 다음 상태: kWaitServerAuthSwitch
        // ---------------------------------------------------------------
        case HandshakeState::kWaitClientAuthSwitch: {
            return HandshakeTransition{.next_state = HandshakeState::kWaitServerAuthSwitch,
                                       .action = HandshakeAction::kRelayToServer};
        }

        // ---------------------------------------------------------------
        // kWaitServerAuthSwitch: AuthSwitch 후 서버 응답 대기
        //   → OK/ERR/AuthMoreData로 분기 (AuthSwitch 중첩은 에러)
        // ---------------------------------------------------------------
        case HandshakeState::kWaitServerAuthSwitch: {
            const auto auth_type = classify_auth_response(payload);
            switch (auth_type) {
                case AuthResponseType::kOk:
                    return HandshakeTransition{.next_state = HandshakeState::kDone,
                                               .action = HandshakeAction::kComplete};
                case AuthResponseType::kError:
                case AuthResponseType::kEof:
                    return HandshakeTransition{.next_state = HandshakeState::kFailed,
                                               .action = HandshakeAction::kTerminate};
                case AuthResponseType::kAuthMoreData:
                    // AuthSwitch 후 AuthMoreData → 추가 라운드트립
                    if (round_trips >= kMaxRoundTrips) {
                        return std::unexpected(
                            ParseError{.code = ParseErrorCode::kMalformedPacket,
                                       .message = "handshake auth loop exceeded max round trips",
                                       .context = std::format("round_trips={}", round_trips)});
                    }
                    return HandshakeTransition{.next_state = HandshakeState::kWaitClientMoreData,
                                               .action = HandshakeAction::kRelayToClient};
                case AuthResponseType::kAuthSwitch:
                    // AuthSwitch 중첩 → fail-close
                    return std::unexpected(
                        ParseError{.code = ParseErrorCode::kMalformedPacket,
                                   .message = "unexpected AuthSwitchRequest after AuthSwitch",
                                   .context = {}});
                case AuthResponseType::kUnknown:
                    return HandshakeTransition{.next_state = HandshakeState::kFailed,
                                               .action = HandshakeAction::kTerminateNoRelay};
            }
            return std::unexpected(ParseError{
                .code = ParseErrorCode::kInternalError,
                .message = "unreachable: classify_auth_response in kWaitServerAuthSwitch",
                .context = {}});
        }

        // ---------------------------------------------------------------
        // kWaitClientMoreData: AuthMoreData 후 클라이언트 응답 대기
        //   → 무조건 서버에 릴레이, 다음 상태: kWaitServerMoreData
        // ---------------------------------------------------------------
        case HandshakeState::kWaitClientMoreData: {
            return HandshakeTransition{.next_state = HandshakeState::kWaitServerMoreData,
                                       .action = HandshakeAction::kRelayToServer};
        }

        // ---------------------------------------------------------------
        // kWaitServerMoreData: AuthMoreData 후 서버 응답 대기
        //   → OK/ERR/AuthMoreData로 분기 (AuthSwitch는 에러)
        // ---------------------------------------------------------------
        case HandshakeState::kWaitServerMoreData: {
            const auto auth_type = classify_auth_response(payload);
            switch (auth_type) {
                case AuthResponseType::kOk:
                    return HandshakeTransition{.next_state = HandshakeState::kDone,
                                               .action = HandshakeAction::kComplete};
                case AuthResponseType::kError:
                case AuthResponseType::kEof:
                    return HandshakeTransition{.next_state = HandshakeState::kFailed,
                                               .action = HandshakeAction::kTerminate};
                case AuthResponseType::kAuthMoreData:
                    // 추가 라운드트립 — 무한 루프 방지
                    if (round_trips >= kMaxRoundTrips) {
                        return std::unexpected(
                            ParseError{.code = ParseErrorCode::kMalformedPacket,
                                       .message = "handshake auth loop exceeded max round trips",
                                       .context = std::format("round_trips={}", round_trips)});
                    }
                    {
                        // fast auth OK(0x03): 서버가 이미 검증 완료 → 서버 OK 패킷 대기
                        // full auth(0x04) 또는 기타: 클라이언트 응답 대기
                        const bool fast_auth_ok = (payload.size() >= 2 && payload[1] == 0x03U);
                        return HandshakeTransition{
                            .next_state = fast_auth_ok ? HandshakeState::kWaitServerMoreData
                                                       : HandshakeState::kWaitClientMoreData,
                            .action = HandshakeAction::kRelayToClient};
                    }
                case AuthResponseType::kAuthSwitch:
                    // AuthMoreData 중 AuthSwitch는 비정상 → fail-close
                    return std::unexpected(
                        ParseError{.code = ParseErrorCode::kMalformedPacket,
                                   .message = "unexpected AuthSwitchRequest after AuthMoreData",
                                   .context = {}});
                case AuthResponseType::kUnknown:
                    // caching_sha2_password RSA 공개키 교환:
                    //   MySQL이 RSA 공개키를 0x01 AuthMoreData 헤더 없이 raw 패킷으로 전송.
                    //   첫 바이트가 '-'(0x2D)이므로 kUnknown으로 분류되지만 정상 프로토콜임.
                    //   클라이언트에 릴레이 후 RSA-암호화 비밀번호를 기다린다.
                    if (round_trips >= kMaxRoundTrips) {
                        return std::unexpected(
                            ParseError{.code = ParseErrorCode::kMalformedPacket,
                                       .message = "handshake auth loop exceeded max round trips",
                                       .context = std::format("round_trips={}", round_trips)});
                    }
                    return HandshakeTransition{.next_state = HandshakeState::kWaitClientMoreData,
                                               .action = HandshakeAction::kRelayToClient};
            }
            return std::unexpected(
                ParseError{.code = ParseErrorCode::kInternalError,
                           .message = "unreachable: classify_auth_response in kWaitServerMoreData",
                           .context = {}});
        }

        // ---------------------------------------------------------------
        // 종단 상태: 이미 완료 또는 실패 — 추가 패킷 처리 불가
        // ---------------------------------------------------------------
        case HandshakeState::kDone:
        case HandshakeState::kFailed:
            return std::unexpected(
                ParseError{.code = ParseErrorCode::kInternalError,
                           .message = "process_handshake_packet called in terminal state",
                           .context = std::format("state={}", static_cast<int>(current_state))});
    }

    // unreachable
    return std::unexpected(ParseError{.code = ParseErrorCode::kInternalError,
                                      .message = "unreachable: unknown HandshakeState",
                                      .context = {}});
}

// ---------------------------------------------------------------------------
// extract_handshake_response_fields  (강화된 버전 — Major 2)
//
//   HandshakeResponse41 레이아웃 (CLIENT_PROTOCOL_41 기준):
//     capability flags : 4 bytes
//     max_packet_size  : 4 bytes
//     charset          : 1 byte
//     reserved         : 23 bytes (zero padding)
//     --- 위까지 합산 offset=32 ---
//     username         : null-terminated string
//     auth_response    : length-encoded string (if CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)
//                        OR 1바이트 length + 데이터 (if CLIENT_SECURE_CONNECTION)
//                        OR null-terminated (if neither)
//     db_name          : null-terminated string (CLIENT_CONNECT_WITH_DB 플래그 시)
// ---------------------------------------------------------------------------
auto extract_handshake_response_fields(std::span<const std::uint8_t> payload,
                                       std::string& out_user,
                                       std::string& out_db) noexcept
    -> std::expected<void, ParseError> {
    // capability flags 4바이트 + 고정 필드 최소 32바이트 + username null terminator
    // 최소: 32바이트 고정 + 적어도 1바이트(username null terminator)
    if (payload.size() < 33) {
        return std::unexpected(
            ParseError{.code = ParseErrorCode::kMalformedPacket,
                       .message = "handshake response payload too short",
                       .context = std::format("payload size={}, need >= 33", payload.size())});
    }

    // capability flags (4바이트 LE)
    const std::uint32_t cap_flags = static_cast<std::uint32_t>(payload[0]) |
                                    (static_cast<std::uint32_t>(payload[1]) << 8U) |
                                    (static_cast<std::uint32_t>(payload[2]) << 16U) |
                                    (static_cast<std::uint32_t>(payload[3]) << 24U);

    // CLIENT_CONNECT_WITH_DB = 0x00000008
    static constexpr std::uint32_t client_connect_with_db = 0x00000008U;
    // CLIENT_SECURE_CONNECTION = 0x00008000
    static constexpr std::uint32_t client_secure_connection = 0x00008000U;
    // CLIENT_PLUGIN_AUTH_LENENC = 0x00200000
    static constexpr std::uint32_t client_plugin_auth_lenenc = 0x00200000U;

    // offset 32부터 username null-terminated string
    std::size_t pos = 32;

    // username 추출: null terminator 탐색
    const std::size_t user_start = pos;
    while (pos < payload.size() && payload[pos] != 0x00) {
        ++pos;
    }

    // username null terminator 존재 확인
    if (pos >= payload.size()) {
        return std::unexpected(
            ParseError{.code = ParseErrorCode::kMalformedPacket,
                       .message = "username missing null terminator in handshake response",
                       .context = std::format("pos={}, payload_size={}", pos, payload.size())});
    }

    out_user.assign(
        reinterpret_cast<const char*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            payload.data() + user_start),
        pos - user_start);

    // null terminator 건너뜀
    ++pos;

    // auth_response 건너뜀
    if ((cap_flags & client_plugin_auth_lenenc) != 0U) {
        // length-encoded integer
        if (pos >= payload.size()) {
            return std::unexpected(
                ParseError{.code = ParseErrorCode::kMalformedPacket,
                           .message = "auth_response length prefix missing",
                           .context = std::format("pos={}, payload_size={}", pos, payload.size())});
        }

        const std::uint8_t len_byte = payload[pos];
        ++pos;

        std::size_t auth_len = 0;
        if (len_byte < 0xFB) {
            // 1바이트 정수
            auth_len = len_byte;
        } else if (len_byte == 0xFC) {
            // 0xFC: 다음 2바이트
            if (pos + 1 >= payload.size()) {
                return std::unexpected(ParseError{
                    .code = ParseErrorCode::kMalformedPacket,
                    .message = "auth_response lenenc 0xFC truncated",
                    .context = std::format("pos={}, payload_size={}", pos, payload.size())});
            }
            auth_len = static_cast<std::size_t>(payload[pos]) |
                       (static_cast<std::size_t>(payload[pos + 1]) << 8U);
            pos += 2;
        } else if (len_byte == 0xFD) {
            // 0xFD: 다음 3바이트
            if (pos + 2 >= payload.size()) {
                return std::unexpected(ParseError{
                    .code = ParseErrorCode::kMalformedPacket,
                    .message = "auth_response lenenc 0xFD truncated",
                    .context = std::format("pos={}, payload_size={}", pos, payload.size())});
            }
            auth_len = static_cast<std::size_t>(payload[pos]) |
                       (static_cast<std::size_t>(payload[pos + 1]) << 8U) |
                       (static_cast<std::size_t>(payload[pos + 2]) << 16U);
            pos += 3;
        } else {
            // 0xFE(8바이트 lenenc) 또는 0xFF(null 표현)는 auth_response에서 비정상
            return std::unexpected(
                ParseError{.code = ParseErrorCode::kMalformedPacket,
                           .message = "auth_response lenenc uses invalid variant (0xFE/0xFF)",
                           .context = std::format("len_byte=0x{:02X}", len_byte)});
        }

        // auth_len이 남은 payload를 초과하는지 검증
        if (auth_len > payload.size() - pos) {
            return std::unexpected(ParseError{
                .code = ParseErrorCode::kMalformedPacket,
                .message = "auth_response length exceeds remaining payload",
                .context =
                    std::format("auth_len={}, remaining={}", auth_len, payload.size() - pos)});
        }
        pos += auth_len;

    } else if ((cap_flags & client_secure_connection) != 0U) {
        // 1바이트 length + 데이터
        if (pos >= payload.size()) {
            return std::unexpected(
                ParseError{.code = ParseErrorCode::kMalformedPacket,
                           .message = "auth_response length prefix missing",
                           .context = std::format("pos={}, payload_size={}", pos, payload.size())});
        }
        const std::size_t auth_len = payload[pos];
        ++pos;

        // auth_len이 남은 payload를 초과하는지 검증
        if (auth_len > payload.size() - pos) {
            return std::unexpected(ParseError{
                .code = ParseErrorCode::kMalformedPacket,
                .message = "auth_response (secure) length exceeds remaining payload",
                .context =
                    std::format("auth_len={}, remaining={}", auth_len, payload.size() - pos)});
        }
        pos += auth_len;

    } else {
        // null-terminated
        while (pos < payload.size() && payload[pos] != 0x00) {
            ++pos;
        }
        if (pos >= payload.size()) {
            return std::unexpected(
                ParseError{.code = ParseErrorCode::kMalformedPacket,
                           .message = "auth_response missing null terminator in handshake response",
                           .context = std::format("pos={}, payload_size={}", pos, payload.size())});
        }
        ++pos;  // null terminator 건너뜀
    }

    // db_name 추출 (CLIENT_CONNECT_WITH_DB가 설정된 경우)
    if ((cap_flags & client_connect_with_db) != 0U) {
        if (pos >= payload.size()) {
            return std::unexpected(
                ParseError{.code = ParseErrorCode::kMalformedPacket,
                           .message = "database field missing despite CLIENT_CONNECT_WITH_DB flag",
                           .context = std::format("pos={}, payload_size={}", pos, payload.size())});
        }

        const std::size_t db_start = pos;
        while (pos < payload.size() && payload[pos] != 0x00) {
            ++pos;
        }

        // db_name null terminator 확인
        if (pos >= payload.size()) {
            return std::unexpected(
                ParseError{.code = ParseErrorCode::kMalformedPacket,
                           .message = "db_name missing null terminator in handshake response",
                           .context = std::format("pos={}, payload_size={}", pos, payload.size())});
        }

        out_db.assign(
            reinterpret_cast<const char*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                payload.data() + db_start),
            pos - db_start);
    } else {
        out_db.clear();
    }

    return std::expected<void, ParseError>{};
}

}  // namespace detail

// ===========================================================================
// HandshakeRelay — public 헬퍼 함수 구현
// ===========================================================================

// static
auto HandshakeRelay::is_ssl_request(std::span<const std::uint8_t> payload) noexcept -> bool {
    // SSLRequest: payload 정확히 32바이트 AND CLIENT_SSL bit(0x0800) set
    if (payload.size() != 32) {
        return false;
    }
    // capability_flags는 4바이트 LE (bytes[0..3])
    const std::uint32_t cap_flags = static_cast<std::uint32_t>(payload[0]) |
                                    (static_cast<std::uint32_t>(payload[1]) << 8U) |
                                    (static_cast<std::uint32_t>(payload[2]) << 16U) |
                                    (static_cast<std::uint32_t>(payload[3]) << 24U);
    return (cap_flags & 0x00000800U) != 0U;  // CLIENT_SSL
}

// static
auto HandshakeRelay::build_ssl_request(std::uint8_t seq_id,
                                       std::uint32_t capability_flags,
                                       std::uint32_t max_packet_size,
                                       std::uint8_t charset) -> std::vector<std::uint8_t> {
    // MySQL SSLRequest 패킷:
    //   4B 헤더 (payload=32, seq_id) + 32B payload
    std::vector<std::uint8_t> buf(4 + 32, 0x00U);

    // 헤더: payload 길이 = 32 (3B LE) + seq_id
    buf[0] = 0x20U;  // 32
    buf[1] = 0x00U;
    buf[2] = 0x00U;
    buf[3] = seq_id;

    // payload: capability_flags(4) + max_packet_size(4) + charset(1) + reserved(23)
    buf[4] = static_cast<std::uint8_t>(capability_flags & 0xFFU);
    buf[5] = static_cast<std::uint8_t>((capability_flags >> 8U) & 0xFFU);
    buf[6] = static_cast<std::uint8_t>((capability_flags >> 16U) & 0xFFU);
    buf[7] = static_cast<std::uint8_t>((capability_flags >> 24U) & 0xFFU);

    buf[8] = static_cast<std::uint8_t>(max_packet_size & 0xFFU);
    buf[9] = static_cast<std::uint8_t>((max_packet_size >> 8U) & 0xFFU);
    buf[10] = static_cast<std::uint8_t>((max_packet_size >> 16U) & 0xFFU);
    buf[11] = static_cast<std::uint8_t>((max_packet_size >> 24U) & 0xFFU);

    buf[12] = charset;
    // buf[13..35] = reserved (already zero)

    return buf;
}

namespace {

// ===========================================================================
// handle_backend_ssl_greeting_phase
//   state=kWaitServerGreeting + has_backend_ssl 경로를 전담한다.
//
//   1. 서버 greeting 읽기 → CLIENT_SSL 지원 검증
//   2. 클라이언트에 greeting relay (frontend SSL 여부 반영)
//   3. 클라이언트 응답 읽기 (Frontend SSL이면 SSLRequest 처리 후 재읽기)
//   4. HandshakeResponse41에서 username/db 추출
//   5. SSLRequest 생성 → 서버 전송
//   6. server_stream TLS 업그레이드
//   7. HandshakeResponse41을 서버에 relay (seq_id=2, CLIENT_SSL 강제 SET)
//
//   성공 시 다음 상태(kWaitServerAuth) 반환.
// ===========================================================================
auto handle_backend_ssl_greeting_phase(AsyncStream& client_stream,
                                       AsyncStream& server_stream,
                                       bool has_frontend_ssl,
                                       const HandshakeSSLConfig& ssl_config,
                                       SslUpgradeState& ssl,
                                       std::string& extracted_user,
                                       std::string& extracted_db,
                                       bool& fields_extracted)
    -> boost::asio::awaitable<std::expected<detail::HandshakeState, ParseError>> {
    // 1. 서버 greeting 읽기
    auto pkt_result = co_await read_packet(server_stream);
    if (!pkt_result) {
        co_return std::unexpected(pkt_result.error());
    }
    const MysqlPacket& greeting_pkt = *pkt_result;
    const auto greeting_payload = greeting_pkt.payload();

    // 서버 greeting에서 capability_flags 추출하여 CLIENT_SSL 지원 확인
    bool server_supports_ssl = false;
    {
        std::size_t pos = 1;
        while (pos < greeting_payload.size() && greeting_payload[pos] != 0x00) {
            ++pos;
        }
        if (pos < greeting_payload.size()) {
            ++pos;      // NUL 건너뜀
            pos += 13;  // conn_id(4) + auth_data_1(8) + filler(1)
            if (pos + 4 <= greeting_payload.size()) {
                const std::uint32_t cap1 =
                    static_cast<std::uint32_t>(greeting_payload[pos]) |
                    (static_cast<std::uint32_t>(greeting_payload[pos + 1]) << 8U);
                std::uint32_t cap2 = 0U;
                if (pos + 4 + 4 <= greeting_payload.size()) {
                    cap2 = static_cast<std::uint32_t>(greeting_payload[pos + 5]) |
                           (static_cast<std::uint32_t>(greeting_payload[pos + 6]) << 8U);
                }
                server_supports_ssl = ((cap1 | (cap2 << 16U)) & 0x00000800U) != 0U;
            }
        }
    }

    if (!server_supports_ssl) {
        spdlog::error(
            "[handshake] backend SSL requested but server does not advertise "
            "CLIENT_SSL — fail-close, rejecting connection");
        co_return std::unexpected(
            ParseError{.code = ParseErrorCode::kInternalError,
                       .message = "backend SSL required but server does not support CLIENT_SSL",
                       .context = {}});
    }
    spdlog::debug("[handshake] backend SSL: server supports CLIENT_SSL, sending SSLRequest");

    // 2. 서버 greeting을 클라이언트에 relay
    const auto modified_greeting = strip_unsupported_capabilities(greeting_pkt, has_frontend_ssl);
    auto wr = co_await write_raw_bytes(client_stream, modified_greeting);
    if (!wr) {
        co_return std::unexpected(wr.error());
    }

    // 3. 클라이언트 응답 읽기
    auto client_pkt_result = co_await read_packet(client_stream);
    if (!client_pkt_result) {
        co_return std::unexpected(client_pkt_result.error());
    }

    // Frontend SSL: SSLRequest이면 TLS 업그레이드 후 진짜 HandshakeResponse41 재읽기
    if (has_frontend_ssl && HandshakeRelay::is_ssl_request(client_pkt_result->payload())) {
        spdlog::debug("[handshake] frontend SSL: SSLRequest received, upgrading client");
        auto fe_tls = co_await setup_frontend_tls(client_stream, *ssl_config.frontend_ssl_ctx);
        if (!fe_tls) {
            co_return std::unexpected(fe_tls.error());
        }
        ssl.frontend = true;
        spdlog::debug("[handshake] frontend TLS handshake succeeded");
        client_pkt_result = co_await read_packet(client_stream);
        if (!client_pkt_result) {
            co_return std::unexpected(client_pkt_result.error());
        }
    }

    // 4. username/db 추출
    const auto client_payload = client_pkt_result->payload();
    auto extract_result =
        detail::extract_handshake_response_fields(client_payload, extracted_user, extracted_db);
    if (!extract_result) {
        co_return std::unexpected(extract_result.error());
    }
    fields_extracted = true;

    // 5. SSLRequest 생성 및 전송
    if (client_payload.size() < 32) {
        co_return std::unexpected(
            ParseError{.code = ParseErrorCode::kInternalError,
                       .message = "HandshakeResponse41 too short for SSL upgrade",
                       .context = {}});
    }

    // SSLRequest = 4-byte header + HandshakeResponse41 payload 첫 32바이트
    std::vector<std::uint8_t> ssl_req(4 + 32, 0U);
    ssl_req[0] = 32U;
    ssl_req[1] = 0U;
    ssl_req[2] = 0U;
    ssl_req[3] = 1U;  // seq_id = 1
    std::copy_n(client_payload.begin(), 32, ssl_req.begin() + 4);
    ssl_req[4 + 1] |= 0x08U;                              // CLIENT_SSL 강제 SET
    ssl_req[4 + 3] &= static_cast<std::uint8_t>(~0x09U);  // EOF/QUERY_ATTR 제거

    boost::system::error_code ssl_req_ec;
    co_await boost::asio::async_write(
        server_stream,
        boost::asio::buffer(ssl_req),
        boost::asio::redirect_error(boost::asio::use_awaitable, ssl_req_ec));
    if (ssl_req_ec) {
        co_return std::unexpected(ParseError{.code = ParseErrorCode::kInternalError,
                                             .message = "failed to send SSLRequest to server",
                                             .context = ssl_req_ec.message()});
    }

    // 6. server_stream TLS 업그레이드
    auto tls_result = co_await setup_backend_tls(server_stream,
                                                 *ssl_config.backend_ssl_ctx,
                                                 ssl_config.backend_ssl_verify,
                                                 ssl_config.backend_tls_server_name);
    if (!tls_result) {
        co_return std::unexpected(tls_result.error());
    }
    ssl.backend = true;
    spdlog::debug("[handshake] backend TLS handshake succeeded");

    // 7. HandshakeResponse41 릴레이 (seq_id=2, CLIENT_SSL 강제 SET)
    auto resp_bytes = client_pkt_result->serialize();
    if (resp_bytes.size() >= 4) {
        resp_bytes[3] = 2;  // seq_id = 2
    }
    if (resp_bytes.size() >= 6) {
        resp_bytes[4 + 1] |= 0x08U;  // CLIENT_SSL 강제 SET
    }
    if (resp_bytes.size() >= 8) {
        resp_bytes[4 + 3] &= static_cast<std::uint8_t>(~0x09U);  // EOF/QUERY_ATTR 제거
    }

    boost::system::error_code resp_ec;
    co_await boost::asio::async_write(
        server_stream,
        boost::asio::buffer(resp_bytes),
        boost::asio::redirect_error(boost::asio::use_awaitable, resp_ec));
    if (resp_ec) {
        co_return std::unexpected(
            ParseError{.code = ParseErrorCode::kInternalError,
                       .message = "failed to relay HandshakeResponse41 to server",
                       .context = resp_ec.message()});
    }

    co_return detail::HandshakeState::kWaitServerAuth;
}

// ===========================================================================
// handle_frontend_ssl_response
//   state=kWaitClientResponse + has_frontend_ssl + SSLRequest 수신 경로를 전담한다.
//
//   1. client_stream TLS 업그레이드
//   2. 진짜 HandshakeResponse41 읽기
//   3. username/db 추출
//   4. capability 정리 후 seq_id delta 적용하여 서버에 relay
//
//   성공 시 다음 상태(kWaitServerAuth) 반환.
// ===========================================================================
auto handle_frontend_ssl_response(AsyncStream& client_stream,
                                  AsyncStream& server_stream,
                                  const HandshakeSSLConfig& ssl_config,
                                  SslUpgradeState& ssl,
                                  std::string& extracted_user,
                                  std::string& extracted_db,
                                  bool& fields_extracted)
    -> boost::asio::awaitable<std::expected<detail::HandshakeState, ParseError>> {
    spdlog::debug("[handshake] frontend SSL: SSLRequest received, upgrading client");
    auto tls_result = co_await setup_frontend_tls(client_stream, *ssl_config.frontend_ssl_ctx);
    if (!tls_result) {
        co_return std::unexpected(tls_result.error());
    }
    ssl.frontend = true;
    spdlog::debug("[handshake] frontend TLS handshake succeeded");

    auto real_pkt_result = co_await read_packet(client_stream);
    if (!real_pkt_result) {
        co_return std::unexpected(real_pkt_result.error());
    }

    const auto real_payload = real_pkt_result->payload();
    auto extract_result =
        detail::extract_handshake_response_fields(real_payload, extracted_user, extracted_db);
    if (!extract_result) {
        co_return std::unexpected(extract_result.error());
    }
    fields_extracted = true;

    auto modified = strip_unsupported_client_capabilities(*real_pkt_result, ssl.backend);
    auto res = co_await relay_with_delta(server_stream,
                                         std::move(modified),
                                         ssl.client_to_server_delta(),
                                         "failed to relay HandshakeResponse41 after SSL");
    if (!res) {
        co_return std::unexpected(res.error());
    }

    co_return detail::HandshakeState::kWaitServerAuth;
}

}  // namespace

// ===========================================================================
// HandshakeRelay::relay_handshake — 얇은 I/O 껍질
//
// 상태 판단 로직은 전부 detail::process_handshake_packet에 위임한다.
// 이 함수는 소켓 read/write + 순수 함수 호출 + MySQL 프로토콜 SSL 업그레이드를 담당한다.
//
// MySQL 프로토콜 레벨 SSL 업그레이드 흐름:
//
//   [Backend SSL]
//     1. 서버 greeting 수신 후, backend SSL 설정이 있고 서버가 CLIENT_SSL 지원 시:
//        SSLRequest 전송 → server_stream.upgrade_to_ssl() → async_handshake()
//     2. 이후 서버에 클라이언트 greeting을 relay (CLIENT_SSL 비트 포함)
//
//   [Frontend SSL]
//     1. 클라이언트에 서버 greeting relay 시 CLIENT_SSL 비트 유지
//     2. 클라이언트로부터 32바이트 SSLRequest 수신 확인 (is_ssl_request)
//     3. client_stream.upgrade_to_ssl() → async_handshake()
//     4. 진짜 HandshakeResponse41 읽기
//     5. 이후 기존 흐름으로 서버에 relay
// ===========================================================================

// static
auto HandshakeRelay::relay_handshake(AsyncStream& client_stream,
                                     AsyncStream& server_stream,
                                     SessionContext& ctx,
                                     const HandshakeSSLConfig& ssl_config)
    -> boost::asio::awaitable<std::expected<void, ParseError>> {
    const bool has_frontend_ssl = (ssl_config.frontend_ssl_ctx != nullptr);
    const bool has_backend_ssl = (ssl_config.backend_ssl_ctx != nullptr);

    detail::HandshakeState state = detail::HandshakeState::kWaitServerGreeting;
    int round_trips = 0;

    std::string extracted_user;
    std::string extracted_db;
    bool fields_extracted = false;
    SslUpgradeState ssl;

    // -----------------------------------------------------------------------
    // 패킷 릴레이 루프
    //
    // 상태 머신 기반으로 패킷을 읽고 → 순수 함수로 전이 판단 → 소켓에 쓴다.
    // kDone 또는 kFailed 상태가 될 때까지 반복한다.
    // -----------------------------------------------------------------------
    while (state != detail::HandshakeState::kDone && state != detail::HandshakeState::kFailed) {
        // 현재 상태에 따라 읽을 소켓 결정
        const bool read_from_server = (state == detail::HandshakeState::kWaitServerGreeting ||
                                       state == detail::HandshakeState::kWaitServerAuth ||
                                       state == detail::HandshakeState::kWaitServerAuthSwitch ||
                                       state == detail::HandshakeState::kWaitServerMoreData);

        AsyncStream& src_stream = read_from_server ? server_stream : client_stream;

        // ───────────────────────────────────────────────────────────────────
        // Backend SSL: 서버 greeting 라운드트립 전체를 헬퍼에 위임
        // ───────────────────────────────────────────────────────────────────
        if (state == detail::HandshakeState::kWaitServerGreeting && has_backend_ssl) {
            auto result = co_await handle_backend_ssl_greeting_phase(client_stream,
                                                                     server_stream,
                                                                     has_frontend_ssl,
                                                                     ssl_config,
                                                                     ssl,
                                                                     extracted_user,
                                                                     extracted_db,
                                                                     fields_extracted);
            if (!result) {
                co_return std::unexpected(result.error());
            }
            state = *result;
            continue;
        }

        // 패킷 읽기 (일반 경로)
        auto pkt_result = co_await read_packet(src_stream);
        if (!pkt_result) {
            co_return std::unexpected(pkt_result.error());
        }

        const MysqlPacket& pkt = *pkt_result;
        const auto payload = pkt.payload();

        // ───────────────────────────────────────────────────────────────────
        // Frontend SSL: 클라이언트가 SSLRequest를 보낸 경우 헬퍼에 위임
        // ───────────────────────────────────────────────────────────────────
        if (state == detail::HandshakeState::kWaitClientResponse && has_frontend_ssl &&
            is_ssl_request(payload)) {
            auto result = co_await handle_frontend_ssl_response(client_stream,
                                                                server_stream,
                                                                ssl_config,
                                                                ssl,
                                                                extracted_user,
                                                                extracted_db,
                                                                fields_extracted);
            if (!result) {
                co_return std::unexpected(result.error());
            }
            state = *result;
            continue;
        }

        // 클라이언트 HandshakeResponse에서 username/db 추출 (일반 경로)
        if (state == detail::HandshakeState::kWaitClientResponse && !fields_extracted) {
            auto extract_result =
                detail::extract_handshake_response_fields(payload, extracted_user, extracted_db);
            if (!extract_result) {
                co_return std::unexpected(extract_result.error());
            }
            fields_extracted = true;
        }

        auto transition_result = detail::process_handshake_packet(state, payload, round_trips);
        if (!transition_result) {
            co_return std::unexpected(transition_result.error());
        }

        const detail::HandshakeTransition& transition = *transition_result;

        // 액션 수행
        switch (transition.action) {
            case detail::HandshakeAction::kRelayToClient: {
                if (state == detail::HandshakeState::kWaitServerGreeting) {
                    // backend SSL 없는 경우의 greeting relay
                    const auto modified = strip_unsupported_capabilities(pkt, has_frontend_ssl);
                    auto wr = co_await write_raw_bytes(client_stream, modified);
                    if (!wr) {
                        co_return std::unexpected(wr.error());
                    }
                } else {
                    auto res = co_await relay_with_delta(client_stream,
                                                         pkt.serialize(),
                                                         ssl.server_to_client_delta(),
                                                         "failed to relay server auth packet");
                    if (!res) {
                        co_return std::unexpected(res.error());
                    }
                }
                break;
            }
            case detail::HandshakeAction::kRelayToServer: {
                if (state == detail::HandshakeState::kWaitClientResponse) {
                    auto modified = strip_unsupported_client_capabilities(pkt, ssl.backend);
                    auto res = co_await relay_with_delta(
                        server_stream,
                        std::move(modified),
                        ssl.client_to_server_delta(),
                        "failed to write modified client handshake response");
                    if (!res) {
                        co_return std::unexpected(res.error());
                    }
                } else {
                    auto res = co_await relay_with_delta(server_stream,
                                                         pkt.serialize(),
                                                         ssl.client_to_server_delta(),
                                                         "failed to relay client auth packet");
                    if (!res) {
                        co_return std::unexpected(res.error());
                    }
                }
                break;
            }
            case detail::HandshakeAction::kComplete: {
                auto res = co_await relay_with_delta(client_stream,
                                                     pkt.serialize(),
                                                     ssl.server_to_client_delta(),
                                                     "failed to relay OK packet");
                if (!res) {
                    co_return std::unexpected(res.error());
                }
                ctx.db_user = extracted_user;
                ctx.db_name = extracted_db;
                ctx.handshake_done = true;
                co_return std::expected<void, ParseError>{};
            }
            case detail::HandshakeAction::kTerminate: {
                co_await write_packet(client_stream, pkt);
                co_return std::unexpected(ParseError{
                    .code = ParseErrorCode::kMalformedPacket,
                    .message = "handshake auth failed",
                    .context =
                        std::format("state={}, payload[0]=0x{:02X}",
                                    static_cast<int>(state),
                                    payload.empty() ? 0U : static_cast<unsigned>(payload[0]))});
            }
            case detail::HandshakeAction::kTerminateNoRelay: {
                co_return std::unexpected(ParseError{
                    .code = ParseErrorCode::kMalformedPacket,
                    .message = "unknown auth response packet type",
                    .context =
                        std::format("state={}, payload[0]=0x{:02X}",
                                    static_cast<int>(state),
                                    payload.empty() ? 0U : static_cast<unsigned>(payload[0]))});
            }
        }

        // AuthMoreData/AuthSwitch 라운드트립 카운터 증가
        if (transition.next_state == detail::HandshakeState::kWaitClientMoreData ||
            transition.next_state == detail::HandshakeState::kWaitClientAuthSwitch) {
            ++round_trips;
        }

        // 상태 전이
        state = transition.next_state;
    }

    // kDone은 kComplete 액션에서 이미 반환되므로 여기는 kFailed만 도달
    co_return std::unexpected(ParseError{
        .code = ParseErrorCode::kMalformedPacket, .message = "handshake failed", .context = {}});
}
