#include "protocol/handshake.hpp"
#include "protocol/handshake_detail.hpp"

#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <format>

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

// -----------------------------------------------------------------------
// read_packet
//   4바이트 헤더를 읽어 payload 길이를 파악한 뒤,
//   전체 패킷 바이트를 읽어 MysqlPacket으로 파싱한다.
// -----------------------------------------------------------------------
auto read_packet(boost::asio::ip::tcp::socket& sock)
    -> boost::asio::awaitable<std::expected<MysqlPacket, ParseError>>
{
    // 4바이트 헤더 읽기
    std::array<std::uint8_t, 4> header{};
    boost::system::error_code ec;

    co_await boost::asio::async_read(
        sock,
        boost::asio::buffer(header),
        boost::asio::redirect_error(boost::asio::use_awaitable, ec)
    );

    if (ec) {
        co_return std::unexpected(ParseError{
            ParseErrorCode::kMalformedPacket,
            "failed to read packet header",
            ec.message()
        });
    }

    // payload 길이 파싱 (3바이트 LE)
    const std::uint32_t payload_len =
        static_cast<std::uint32_t>(header[0])
        | (static_cast<std::uint32_t>(header[1]) << 8U)
        | (static_cast<std::uint32_t>(header[2]) << 16U);

    // 전체 패킷 버퍼: 헤더(4) + payload
    std::vector<std::uint8_t> buf(4 + payload_len);
    buf[0] = header[0];
    buf[1] = header[1];
    buf[2] = header[2];
    buf[3] = header[3];

    if (payload_len > 0) {
        co_await boost::asio::async_read(
            sock,
            boost::asio::buffer(buf.data() + 4, payload_len),
            boost::asio::redirect_error(boost::asio::use_awaitable, ec)
        );

        if (ec) {
            co_return std::unexpected(ParseError{
                ParseErrorCode::kMalformedPacket,
                "failed to read packet payload",
                ec.message()
            });
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
//      프록시는 TLS 미지원. 서버가 SSL을 광고하면 mysql 클라이언트가
//      SSLRequest(32B)를 전송해 HandshakeResponse41 파싱 실패.
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
//     [2B  capability_flags_1]  ← CLIENT_SSL(bit 11) 제거
//     [1B  charset]
//     [2B  status_flags]
//     [2B  capability_flags_2]  ← CLIENT_QUERY_ATTRIBUTES(bit 11),
//                                  CLIENT_DEPRECATE_EOF(bit 8) 제거
//     ...
//
//   파싱 실패 시 원본 직렬화 바이트를 반환한다 (fail-safe).
// -----------------------------------------------------------------------
auto strip_unsupported_capabilities(const MysqlPacket& pkt) -> std::vector<std::uint8_t>
{
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
    bytes[cap1_offset + 1] &= static_cast<std::uint8_t>(~0x08U);

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
//   payload layout (offset 0):
//     [4B capability_flags] [4B max_packet_size] [1B charset] [23B reserved] ...
// -----------------------------------------------------------------------
auto strip_unsupported_client_capabilities(const MysqlPacket& pkt)
    -> std::vector<std::uint8_t>
{
    auto bytes = pkt.serialize();
    const auto payload = pkt.payload();

    // HandshakeResponse41 capability_flags(4B) 최소 길이 확인
    if (payload.size() < 4 || bytes.size() < 8) {
        return bytes;
    }

    constexpr std::uint32_t kUnsupportedMask =
        0x00000800U   // CLIENT_SSL
        | 0x01000000U // CLIENT_DEPRECATE_EOF
        | 0x08000000U; // CLIENT_QUERY_ATTRIBUTES

    // serialized bytes에서 payload 시작 오프셋은 4
    std::uint32_t cap_flags =
        static_cast<std::uint32_t>(bytes[4])
        | (static_cast<std::uint32_t>(bytes[5]) << 8U)
        | (static_cast<std::uint32_t>(bytes[6]) << 16U)
        | (static_cast<std::uint32_t>(bytes[7]) << 24U);

    cap_flags &= ~kUnsupportedMask;

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
auto write_packet(boost::asio::ip::tcp::socket& sock, const MysqlPacket& pkt)
    -> boost::asio::awaitable<std::expected<void, ParseError>>
{
    const auto bytes = pkt.serialize();
    boost::system::error_code ec;

    co_await boost::asio::async_write(
        sock,
        boost::asio::buffer(bytes),
        boost::asio::redirect_error(boost::asio::use_awaitable, ec)
    );

    if (ec) {
        co_return std::unexpected(ParseError{
            ParseErrorCode::kInternalError,
            "failed to write packet",
            ec.message()
        });
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
auto classify_auth_response(std::span<const std::uint8_t> payload) noexcept
    -> AuthResponseType
{
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
auto process_handshake_packet(HandshakeState                current_state,
                              std::span<const std::uint8_t> payload,
                              int                           round_trips) noexcept
    -> std::expected<HandshakeTransition, ParseError>
{
    switch (current_state) {

        // ---------------------------------------------------------------
        // kWaitServerGreeting: 서버 Initial Handshake 수신
        //   → 무조건 클라이언트에 릴레이, 다음 상태: kWaitClientResponse
        // ---------------------------------------------------------------
        case HandshakeState::kWaitServerGreeting: {
            if (payload.empty()) {
                return std::unexpected(ParseError{
                    ParseErrorCode::kMalformedPacket,
                    "empty server greeting payload",
                    {}
                });
            }
            return HandshakeTransition{
                HandshakeState::kWaitClientResponse,
                HandshakeAction::kRelayToClient
            };
        }

        // ---------------------------------------------------------------
        // kWaitClientResponse: 클라이언트 HandshakeResponse 수신
        //   → 무조건 서버에 릴레이, 다음 상태: kWaitServerAuth
        // ---------------------------------------------------------------
        case HandshakeState::kWaitClientResponse: {
            // 최소 길이 검증은 extract_handshake_response_fields에서 수행
            return HandshakeTransition{
                HandshakeState::kWaitServerAuth,
                HandshakeAction::kRelayToServer
            };
        }

        // ---------------------------------------------------------------
        // kWaitServerAuth: 서버 첫 번째 auth 응답 수신
        //   → OK/ERR/EOF/AuthSwitch/AuthMoreData로 분기
        // ---------------------------------------------------------------
        case HandshakeState::kWaitServerAuth: {
            const auto auth_type = classify_auth_response(payload);
            switch (auth_type) {
                case AuthResponseType::kOk:
                    return HandshakeTransition{
                        HandshakeState::kDone,
                        HandshakeAction::kComplete
                    };
                case AuthResponseType::kError:
                    return HandshakeTransition{
                        HandshakeState::kFailed,
                        HandshakeAction::kTerminate
                    };
                case AuthResponseType::kEof:
                    return HandshakeTransition{
                        HandshakeState::kFailed,
                        HandshakeAction::kTerminate
                    };
                case AuthResponseType::kAuthSwitch:
                    return HandshakeTransition{
                        HandshakeState::kWaitClientAuthSwitch,
                        HandshakeAction::kRelayToClient
                    };
                case AuthResponseType::kAuthMoreData: {
                    // caching_sha2_password AuthMoreData 분기:
                    //   payload[1] == 0x03: fast auth OK —
                    //     서버가 캐시로 패스워드 검증 완료. 클라이언트 응답 없이
                    //     서버의 최종 OK 패킷만 기다리므로 kWaitServerMoreData로 전이.
                    //   payload[1] == 0x04 or other: full auth needed —
                    //     클라이언트가 RSA 키 교환 등을 수행해야 하므로 kWaitClientMoreData.
                    const bool fast_auth_ok =
                        (payload.size() >= 2 && payload[1] == 0x03U);
                    return HandshakeTransition{
                        fast_auth_ok ? HandshakeState::kWaitServerMoreData
                                     : HandshakeState::kWaitClientMoreData,
                        HandshakeAction::kRelayToClient
                    };
                }
                case AuthResponseType::kUnknown:
                    return HandshakeTransition{
                        HandshakeState::kFailed,
                        HandshakeAction::kTerminateNoRelay
                    };
            }
            // unreachable — 컴파일러 경고 방지
            return std::unexpected(ParseError{
                ParseErrorCode::kInternalError,
                "unreachable: classify_auth_response",
                {}
            });
        }

        // ---------------------------------------------------------------
        // kWaitClientAuthSwitch: AuthSwitch 후 클라이언트 응답 대기
        //   → 무조건 서버에 릴레이, 다음 상태: kWaitServerAuthSwitch
        // ---------------------------------------------------------------
        case HandshakeState::kWaitClientAuthSwitch: {
            return HandshakeTransition{
                HandshakeState::kWaitServerAuthSwitch,
                HandshakeAction::kRelayToServer
            };
        }

        // ---------------------------------------------------------------
        // kWaitServerAuthSwitch: AuthSwitch 후 서버 응답 대기
        //   → OK/ERR/AuthMoreData로 분기 (AuthSwitch 중첩은 에러)
        // ---------------------------------------------------------------
        case HandshakeState::kWaitServerAuthSwitch: {
            const auto auth_type = classify_auth_response(payload);
            switch (auth_type) {
                case AuthResponseType::kOk:
                    return HandshakeTransition{
                        HandshakeState::kDone,
                        HandshakeAction::kComplete
                    };
                case AuthResponseType::kError:
                    return HandshakeTransition{
                        HandshakeState::kFailed,
                        HandshakeAction::kTerminate
                    };
                case AuthResponseType::kEof:
                    return HandshakeTransition{
                        HandshakeState::kFailed,
                        HandshakeAction::kTerminate
                    };
                case AuthResponseType::kAuthMoreData:
                    // AuthSwitch 후 AuthMoreData → 추가 라운드트립
                    if (round_trips >= kMaxRoundTrips) {
                        return std::unexpected(ParseError{
                            ParseErrorCode::kMalformedPacket,
                            "handshake auth loop exceeded max round trips",
                            std::format("round_trips={}", round_trips)
                        });
                    }
                    return HandshakeTransition{
                        HandshakeState::kWaitClientMoreData,
                        HandshakeAction::kRelayToClient
                    };
                case AuthResponseType::kAuthSwitch:
                    // AuthSwitch 중첩 → fail-close
                    return std::unexpected(ParseError{
                        ParseErrorCode::kMalformedPacket,
                        "unexpected AuthSwitchRequest after AuthSwitch",
                        {}
                    });
                case AuthResponseType::kUnknown:
                    return HandshakeTransition{
                        HandshakeState::kFailed,
                        HandshakeAction::kTerminateNoRelay
                    };
            }
            return std::unexpected(ParseError{
                ParseErrorCode::kInternalError,
                "unreachable: classify_auth_response in kWaitServerAuthSwitch",
                {}
            });
        }

        // ---------------------------------------------------------------
        // kWaitClientMoreData: AuthMoreData 후 클라이언트 응답 대기
        //   → 무조건 서버에 릴레이, 다음 상태: kWaitServerMoreData
        // ---------------------------------------------------------------
        case HandshakeState::kWaitClientMoreData: {
            return HandshakeTransition{
                HandshakeState::kWaitServerMoreData,
                HandshakeAction::kRelayToServer
            };
        }

        // ---------------------------------------------------------------
        // kWaitServerMoreData: AuthMoreData 후 서버 응답 대기
        //   → OK/ERR/AuthMoreData로 분기 (AuthSwitch는 에러)
        // ---------------------------------------------------------------
        case HandshakeState::kWaitServerMoreData: {
            const auto auth_type = classify_auth_response(payload);
            switch (auth_type) {
                case AuthResponseType::kOk:
                    return HandshakeTransition{
                        HandshakeState::kDone,
                        HandshakeAction::kComplete
                    };
                case AuthResponseType::kError:
                    return HandshakeTransition{
                        HandshakeState::kFailed,
                        HandshakeAction::kTerminate
                    };
                case AuthResponseType::kEof:
                    return HandshakeTransition{
                        HandshakeState::kFailed,
                        HandshakeAction::kTerminate
                    };
                case AuthResponseType::kAuthMoreData:
                    // 추가 라운드트립 — 무한 루프 방지
                    if (round_trips >= kMaxRoundTrips) {
                        return std::unexpected(ParseError{
                            ParseErrorCode::kMalformedPacket,
                            "handshake auth loop exceeded max round trips",
                            std::format("round_trips={}", round_trips)
                        });
                    }
                    {
                        // fast auth OK(0x03): 서버가 이미 검증 완료 → 서버 OK 패킷 대기
                        // full auth(0x04) 또는 기타: 클라이언트 응답 대기
                        const bool fast_auth_ok =
                            (payload.size() >= 2 && payload[1] == 0x03U);
                        return HandshakeTransition{
                            fast_auth_ok ? HandshakeState::kWaitServerMoreData
                                         : HandshakeState::kWaitClientMoreData,
                            HandshakeAction::kRelayToClient
                        };
                    }
                case AuthResponseType::kAuthSwitch:
                    // AuthMoreData 중 AuthSwitch는 비정상 → fail-close
                    return std::unexpected(ParseError{
                        ParseErrorCode::kMalformedPacket,
                        "unexpected AuthSwitchRequest after AuthMoreData",
                        {}
                    });
                case AuthResponseType::kUnknown:
                    // caching_sha2_password RSA 공개키 교환:
                    //   MySQL이 RSA 공개키를 0x01 AuthMoreData 헤더 없이 raw 패킷으로 전송.
                    //   첫 바이트가 '-'(0x2D)이므로 kUnknown으로 분류되지만 정상 프로토콜임.
                    //   클라이언트에 릴레이 후 RSA-암호화 비밀번호를 기다린다.
                    if (round_trips >= kMaxRoundTrips) {
                        return std::unexpected(ParseError{
                            ParseErrorCode::kMalformedPacket,
                            "handshake auth loop exceeded max round trips",
                            std::format("round_trips={}", round_trips)
                        });
                    }
                    return HandshakeTransition{
                        HandshakeState::kWaitClientMoreData,
                        HandshakeAction::kRelayToClient
                    };
            }
            return std::unexpected(ParseError{
                ParseErrorCode::kInternalError,
                "unreachable: classify_auth_response in kWaitServerMoreData",
                {}
            });
        }

        // ---------------------------------------------------------------
        // 종단 상태: 이미 완료 또는 실패 — 추가 패킷 처리 불가
        // ---------------------------------------------------------------
        case HandshakeState::kDone:
        case HandshakeState::kFailed:
            return std::unexpected(ParseError{
                ParseErrorCode::kInternalError,
                "process_handshake_packet called in terminal state",
                std::format("state={}", static_cast<int>(current_state))
            });
    }

    // unreachable
    return std::unexpected(ParseError{
        ParseErrorCode::kInternalError,
        "unreachable: unknown HandshakeState",
        {}
    });
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
auto extract_handshake_response_fields(
    std::span<const std::uint8_t> payload,
    std::string&                  out_user,
    std::string&                  out_db) noexcept -> std::expected<void, ParseError>
{
    // capability flags 4바이트 + 고정 필드 최소 32바이트 + username null terminator
    // 최소: 32바이트 고정 + 적어도 1바이트(username null terminator)
    if (payload.size() < 33) {
        return std::unexpected(ParseError{
            ParseErrorCode::kMalformedPacket,
            "handshake response payload too short",
            std::format("payload size={}, need >= 33", payload.size())
        });
    }

    // capability flags (4바이트 LE)
    const std::uint32_t cap_flags =
        static_cast<std::uint32_t>(payload[0])
        | (static_cast<std::uint32_t>(payload[1]) << 8U)
        | (static_cast<std::uint32_t>(payload[2]) << 16U)
        | (static_cast<std::uint32_t>(payload[3]) << 24U);

    // CLIENT_CONNECT_WITH_DB = 0x00000008
    static constexpr std::uint32_t kClientConnectWithDb    = 0x00000008U;
    // CLIENT_SECURE_CONNECTION = 0x00008000
    static constexpr std::uint32_t kClientSecureConnection = 0x00008000U;
    // CLIENT_PLUGIN_AUTH_LENENC = 0x00200000
    static constexpr std::uint32_t kClientPluginAuthLenenc = 0x00200000U;

    // offset 32부터 username null-terminated string
    std::size_t pos = 32;

    // username 추출: null terminator 탐색
    const std::size_t user_start = pos;
    while (pos < payload.size() && payload[pos] != 0x00) {
        ++pos;
    }

    // username null terminator 존재 확인
    if (pos >= payload.size()) {
        return std::unexpected(ParseError{
            ParseErrorCode::kMalformedPacket,
            "username missing null terminator in handshake response",
            std::format("pos={}, payload_size={}", pos, payload.size())
        });
    }

    out_user.assign(
        reinterpret_cast<const char*>(payload.data() + user_start),
        pos - user_start
    );

    // null terminator 건너뜀
    ++pos;

    // auth_response 건너뜀
    if ((cap_flags & kClientPluginAuthLenenc) != 0U) {
        // length-encoded integer
        if (pos >= payload.size()) {
            return std::unexpected(ParseError{
                ParseErrorCode::kMalformedPacket,
                "auth_response length prefix missing",
                std::format("pos={}, payload_size={}", pos, payload.size())
            });
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
                    ParseErrorCode::kMalformedPacket,
                    "auth_response lenenc 0xFC truncated",
                    std::format("pos={}, payload_size={}", pos, payload.size())
                });
            }
            auth_len = static_cast<std::size_t>(payload[pos])
                       | (static_cast<std::size_t>(payload[pos + 1]) << 8U);
            pos += 2;
        } else if (len_byte == 0xFD) {
            // 0xFD: 다음 3바이트
            if (pos + 2 >= payload.size()) {
                return std::unexpected(ParseError{
                    ParseErrorCode::kMalformedPacket,
                    "auth_response lenenc 0xFD truncated",
                    std::format("pos={}, payload_size={}", pos, payload.size())
                });
            }
            auth_len = static_cast<std::size_t>(payload[pos])
                       | (static_cast<std::size_t>(payload[pos + 1]) << 8U)
                       | (static_cast<std::size_t>(payload[pos + 2]) << 16U);
            pos += 3;
        } else {
            // 0xFE(8바이트 lenenc) 또는 0xFF(null 표현)는 auth_response에서 비정상
            return std::unexpected(ParseError{
                ParseErrorCode::kMalformedPacket,
                "auth_response lenenc uses invalid variant (0xFE/0xFF)",
                std::format("len_byte=0x{:02X}", len_byte)
            });
        }

        // auth_len이 남은 payload를 초과하는지 검증
        if (auth_len > payload.size() - pos) {
            return std::unexpected(ParseError{
                ParseErrorCode::kMalformedPacket,
                "auth_response length exceeds remaining payload",
                std::format("auth_len={}, remaining={}", auth_len, payload.size() - pos)
            });
        }
        pos += auth_len;

    } else if ((cap_flags & kClientSecureConnection) != 0U) {
        // 1바이트 length + 데이터
        if (pos >= payload.size()) {
            return std::unexpected(ParseError{
                ParseErrorCode::kMalformedPacket,
                "auth_response length prefix missing",
                std::format("pos={}, payload_size={}", pos, payload.size())
            });
        }
        const std::size_t auth_len = payload[pos];
        ++pos;

        // auth_len이 남은 payload를 초과하는지 검증
        if (auth_len > payload.size() - pos) {
            return std::unexpected(ParseError{
                ParseErrorCode::kMalformedPacket,
                "auth_response (secure) length exceeds remaining payload",
                std::format("auth_len={}, remaining={}", auth_len, payload.size() - pos)
            });
        }
        pos += auth_len;

    } else {
        // null-terminated
        while (pos < payload.size() && payload[pos] != 0x00) {
            ++pos;
        }
        if (pos >= payload.size()) {
            return std::unexpected(ParseError{
                ParseErrorCode::kMalformedPacket,
                "auth_response missing null terminator in handshake response",
                std::format("pos={}, payload_size={}", pos, payload.size())
            });
        }
        ++pos;  // null terminator 건너뜀
    }

    // db_name 추출 (CLIENT_CONNECT_WITH_DB가 설정된 경우)
    if ((cap_flags & kClientConnectWithDb) != 0U) {
        if (pos >= payload.size()) {
            return std::unexpected(ParseError{
                ParseErrorCode::kMalformedPacket,
                "database field missing despite CLIENT_CONNECT_WITH_DB flag",
                std::format("pos={}, payload_size={}", pos, payload.size())
            });
        }

        const std::size_t db_start = pos;
        while (pos < payload.size() && payload[pos] != 0x00) {
            ++pos;
        }

        // db_name null terminator 확인
        if (pos >= payload.size()) {
            return std::unexpected(ParseError{
                ParseErrorCode::kMalformedPacket,
                "db_name missing null terminator in handshake response",
                std::format("pos={}, payload_size={}", pos, payload.size())
            });
        }

        out_db.assign(
            reinterpret_cast<const char*>(payload.data() + db_start),
            pos - db_start
        );
    } else {
        out_db.clear();
    }

    return std::expected<void, ParseError>{};
}

}  // namespace detail

// ===========================================================================
// HandshakeRelay::relay_handshake — 얇은 I/O 껍질
//
// 상태 판단 로직은 전부 detail::process_handshake_packet에 위임한다.
// 이 함수는 소켓 read/write + 순수 함수 호출만 담당한다.
// ===========================================================================

// static
auto HandshakeRelay::relay_handshake(
    boost::asio::ip::tcp::socket& client_sock,
    boost::asio::ip::tcp::socket& server_sock,
    SessionContext&               ctx
) -> boost::asio::awaitable<std::expected<void, ParseError>>
{
    detail::HandshakeState state    = detail::HandshakeState::kWaitServerGreeting;
    int                    round_trips = 0;

    std::string extracted_user;
    std::string extracted_db;
    bool        fields_extracted = false;

    // -----------------------------------------------------------------------
    // 패킷 릴레이 루프
    //
    // 상태 머신 기반으로 패킷을 읽고 → 순수 함수로 전이 판단 → 소켓에 쓴다.
    // kDone 또는 kFailed 상태가 될 때까지 반복한다.
    // -----------------------------------------------------------------------
    while (state != detail::HandshakeState::kDone &&
           state != detail::HandshakeState::kFailed)
    {
        // 현재 상태에 따라 읽을 소켓 결정
        const bool read_from_server =
            (state == detail::HandshakeState::kWaitServerGreeting  ||
             state == detail::HandshakeState::kWaitServerAuth       ||
             state == detail::HandshakeState::kWaitServerAuthSwitch ||
             state == detail::HandshakeState::kWaitServerMoreData);

        boost::asio::ip::tcp::socket& src_sock = read_from_server ? server_sock : client_sock;
        boost::asio::ip::tcp::socket& dst_sock = read_from_server ? client_sock : server_sock;
        (void)dst_sock;  // 실제 쓰기는 action에 따라 결정

        // 패킷 읽기
        auto pkt_result = co_await read_packet(src_sock);
        if (!pkt_result) {
            co_return std::unexpected(pkt_result.error());
        }

        const MysqlPacket& pkt = *pkt_result;
        const auto payload = pkt.payload();

        // 클라이언트 HandshakeResponse에서 username/db 추출
        if (state == detail::HandshakeState::kWaitClientResponse && !fields_extracted) {
            auto extract_result = detail::extract_handshake_response_fields(
                payload, extracted_user, extracted_db
            );
            if (!extract_result) {
                co_return std::unexpected(extract_result.error());
            }
            fields_extracted = true;
        }

        // 순수 함수로 상태 전이 판단
        auto transition_result = detail::process_handshake_packet(state, payload, round_trips);
        if (!transition_result) {
            co_return std::unexpected(transition_result.error());
        }

        const detail::HandshakeTransition& transition = *transition_result;

        // 액션 수행
        switch (transition.action) {
            case detail::HandshakeAction::kRelayToClient: {
                if (state == detail::HandshakeState::kWaitServerGreeting) {
                    // Initial Handshake 릴레이: CLIENT_SSL 비트 제거
                    // 프록시는 TLS 미지원 — SSL 광고를 유지하면 클라이언트가
                    // SSLRequest(32B)를 보내 HandshakeResponse41 파싱 실패
                    const auto modified = strip_unsupported_capabilities(pkt);
                    boost::system::error_code write_ec;
                    co_await boost::asio::async_write(
                        client_sock,
                        boost::asio::buffer(modified),
                        boost::asio::redirect_error(boost::asio::use_awaitable, write_ec)
                    );
                    if (write_ec) {
                        co_return std::unexpected(ParseError{
                            ParseErrorCode::kInternalError,
                            "failed to write modified server greeting",
                            write_ec.message()
                        });
                    }
                } else {
                    auto write_result = co_await write_packet(client_sock, pkt);
                    if (!write_result) {
                        co_return std::unexpected(write_result.error());
                    }
                }
                break;
            }
            case detail::HandshakeAction::kRelayToServer: {
                if (state == detail::HandshakeState::kWaitClientResponse) {
                    // HandshakeResponse41 릴레이: 서버 그리팅에서 제거한 capability와
                    // 동일한 비트를 클라이언트 응답에서도 제거해 양방향 정합성 유지
                    const auto modified = strip_unsupported_client_capabilities(pkt);
                    boost::system::error_code write_ec;
                    co_await boost::asio::async_write(
                        server_sock,
                        boost::asio::buffer(modified),
                        boost::asio::redirect_error(boost::asio::use_awaitable, write_ec)
                    );
                    if (write_ec) {
                        co_return std::unexpected(ParseError{
                            ParseErrorCode::kInternalError,
                            "failed to write modified client handshake response",
                            write_ec.message()
                        });
                    }
                } else {
                    auto write_result = co_await write_packet(server_sock, pkt);
                    if (!write_result) {
                        co_return std::unexpected(write_result.error());
                    }
                }
                break;
            }
            case detail::HandshakeAction::kComplete: {
                // OK 패킷을 클라이언트에 전달하고 완료
                auto write_result = co_await write_packet(client_sock, pkt);
                if (!write_result) {
                    co_return std::unexpected(write_result.error());
                }
                // ctx 업데이트
                ctx.db_user        = extracted_user;
                ctx.db_name        = extracted_db;
                ctx.handshake_done = true;
                co_return std::expected<void, ParseError>{};
            }
            case detail::HandshakeAction::kTerminate: {
                // ERR/EOF 패킷을 클라이언트에 전달하고 실패 반환
                co_await write_packet(client_sock, pkt);
                co_return std::unexpected(ParseError{
                    ParseErrorCode::kMalformedPacket,
                    "handshake auth failed",
                    std::format("state={}, payload[0]=0x{:02X}",
                        static_cast<int>(state),
                        payload.empty() ? 0U : static_cast<unsigned>(payload[0]))
                });
            }
            case detail::HandshakeAction::kTerminateNoRelay: {
                // unknown 패킷 — ERR 전달 없이 종료 (fail-close)
                co_return std::unexpected(ParseError{
                    ParseErrorCode::kMalformedPacket,
                    "unknown auth response packet type",
                    std::format("state={}, payload[0]=0x{:02X}",
                        static_cast<int>(state),
                        payload.empty() ? 0U : static_cast<unsigned>(payload[0]))
                });
            }
        }

        // AuthMoreData/AuthSwitch 라운드트립 카운터 증가
        if (transition.next_state == detail::HandshakeState::kWaitClientMoreData ||
            transition.next_state == detail::HandshakeState::kWaitClientAuthSwitch)
        {
            ++round_trips;
        }

        // 상태 전이
        state = transition.next_state;
    }

    // kDone은 kComplete 액션에서 이미 반환되므로 여기는 kFailed만 도달
    co_return std::unexpected(ParseError{
        ParseErrorCode::kMalformedPacket,
        "handshake failed",
        {}
    });
}
