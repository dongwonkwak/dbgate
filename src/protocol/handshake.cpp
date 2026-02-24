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
                case AuthResponseType::kAuthMoreData:
                    return HandshakeTransition{
                        HandshakeState::kWaitClientMoreData,
                        HandshakeAction::kRelayToClient
                    };
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
                    return HandshakeTransition{
                        HandshakeState::kWaitClientMoreData,
                        HandshakeAction::kRelayToClient
                    };
                case AuthResponseType::kAuthSwitch:
                    // AuthMoreData 중 AuthSwitch는 비정상 → fail-close
                    return std::unexpected(ParseError{
                        ParseErrorCode::kMalformedPacket,
                        "unexpected AuthSwitchRequest after AuthMoreData",
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
                auto write_result = co_await write_packet(client_sock, pkt);
                if (!write_result) {
                    co_return std::unexpected(write_result.error());
                }
                break;
            }
            case detail::HandshakeAction::kRelayToServer: {
                auto write_result = co_await write_packet(server_sock, pkt);
                if (!write_result) {
                    co_return std::unexpected(write_result.error());
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
