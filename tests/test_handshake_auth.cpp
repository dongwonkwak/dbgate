// ---------------------------------------------------------------------------
// test_handshake_auth.cpp
//
// handshake auth 응답 분기 및 mysql_packet 상한 검증 단위 테스트 (DON-24)
//
// 검증 대상:
//   Major 1: classify_auth_response — auth 응답 분류 순수 함수
//   Major 1: process_handshake_packet — 상태 머신 전이 순수 함수
//   Major 2: extract_handshake_response_fields — HandshakeResponse 파싱 강화
//   Major 3: serialize() 상한 검증, make_error() truncation
// ---------------------------------------------------------------------------

#include "protocol/handshake_detail.hpp"
#include "protocol/mysql_packet.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// ===========================================================================
// Major 1: classify_auth_response — 순수 함수 직접 테스트
// ===========================================================================

// ---------------------------------------------------------------------------
// CA-1. 0x00 → kOk (핸드셰이크 완료)
// ---------------------------------------------------------------------------
TEST(ClassifyAuthResponse, X00_IsOk) {
    const std::vector<std::uint8_t> payload = {0x00, 0x00, 0x00};
    const auto result = detail::classify_auth_response(
        std::span<const std::uint8_t>{payload}
    );
    EXPECT_EQ(result, detail::AuthResponseType::kOk);
}

// ---------------------------------------------------------------------------
// CA-2. 0xFF → kError (인증 실패)
// ---------------------------------------------------------------------------
TEST(ClassifyAuthResponse, XFF_IsError) {
    const std::vector<std::uint8_t> payload = {0xFF, 0x15, 0x04};
    const auto result = detail::classify_auth_response(
        std::span<const std::uint8_t>{payload}
    );
    EXPECT_EQ(result, detail::AuthResponseType::kError);
}

// ---------------------------------------------------------------------------
// CA-3. 0xFE + payload 8바이트 → kEof (payload < 9)
// ---------------------------------------------------------------------------
TEST(ClassifyAuthResponse, XFE_8bytes_IsEof) {
    // payload[0]=0xFE, 총 8바이트 (< 9) → kEof
    std::vector<std::uint8_t> payload = {
        0xFE, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
    };
    ASSERT_EQ(payload.size(), 8U);
    const auto result = detail::classify_auth_response(
        std::span<const std::uint8_t>{payload}
    );
    EXPECT_EQ(result, detail::AuthResponseType::kEof);
}

// ---------------------------------------------------------------------------
// CA-4. 0xFE + payload 9바이트 → kAuthSwitch (payload >= 9)
// ---------------------------------------------------------------------------
TEST(ClassifyAuthResponse, XFE_9bytes_IsAuthSwitch) {
    // payload[0]=0xFE, 총 9바이트 (== 9) → kAuthSwitch
    std::vector<std::uint8_t> payload = {
        0xFE, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
    };
    ASSERT_EQ(payload.size(), 9U);
    const auto result = detail::classify_auth_response(
        std::span<const std::uint8_t>{payload}
    );
    EXPECT_EQ(result, detail::AuthResponseType::kAuthSwitch);
}

// ---------------------------------------------------------------------------
// CA-5. 0x01 → kAuthMoreData (caching_sha2_password 등)
// ---------------------------------------------------------------------------
TEST(ClassifyAuthResponse, X01_IsAuthMoreData) {
    const std::vector<std::uint8_t> payload = {0x01, 0xAA, 0xBB};
    const auto result = detail::classify_auth_response(
        std::span<const std::uint8_t>{payload}
    );
    EXPECT_EQ(result, detail::AuthResponseType::kAuthMoreData);
}

// ---------------------------------------------------------------------------
// CA-6. 0x02 (unknown) → kUnknown
// ---------------------------------------------------------------------------
TEST(ClassifyAuthResponse, X02_IsUnknown) {
    const std::vector<std::uint8_t> payload = {0x02, 0x00};
    const auto result = detail::classify_auth_response(
        std::span<const std::uint8_t>{payload}
    );
    EXPECT_EQ(result, detail::AuthResponseType::kUnknown);
}

// ---------------------------------------------------------------------------
// CA-7. 빈 payload → kUnknown
// ---------------------------------------------------------------------------
TEST(ClassifyAuthResponse, Empty_IsUnknown) {
    const std::vector<std::uint8_t> payload;
    const auto result = detail::classify_auth_response(
        std::span<const std::uint8_t>{payload}
    );
    EXPECT_EQ(result, detail::AuthResponseType::kUnknown);
}

// ---------------------------------------------------------------------------
// CA-8. 0xFE 단독 (1바이트, < 9) → kEof (경계값)
// ---------------------------------------------------------------------------
TEST(ClassifyAuthResponse, XFE_1byte_IsEof) {
    const std::vector<std::uint8_t> payload = {0xFE};
    const auto result = detail::classify_auth_response(
        std::span<const std::uint8_t>{payload}
    );
    EXPECT_EQ(result, detail::AuthResponseType::kEof);
}

// ---------------------------------------------------------------------------
// CA-9. 0xFE + payload 정확히 10바이트 → kAuthSwitch (payload >= 9)
// ---------------------------------------------------------------------------
TEST(ClassifyAuthResponse, XFE_10bytes_IsAuthSwitch) {
    std::vector<std::uint8_t> payload(10, 0x00);
    payload[0] = 0xFE;
    const auto result = detail::classify_auth_response(
        std::span<const std::uint8_t>{payload}
    );
    EXPECT_EQ(result, detail::AuthResponseType::kAuthSwitch);
}

// ===========================================================================
// Major 1: process_handshake_packet — 상태 머신 전이 순수 함수 직접 테스트
// ===========================================================================

// 헬퍼: 주어진 first_byte로 N바이트 payload 생성
static std::vector<std::uint8_t> make_payload(std::uint8_t first_byte,
                                               std::size_t  total_size = 1)
{
    std::vector<std::uint8_t> payload(total_size, 0x00);
    if (!payload.empty()) {
        payload[0] = first_byte;
    }
    return payload;
}

// ---------------------------------------------------------------------------
// PP-1. 정상 흐름: kWaitServerGreeting → RelayToClient
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerGreeting_RelaysToClient) {
    const auto payload = make_payload(0x0A, 77);  // Initial Handshake
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerGreeting,
        std::span<const std::uint8_t>{payload},
        0
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kWaitClientResponse);
    EXPECT_EQ(result->action, detail::HandshakeAction::kRelayToClient);
}

// ---------------------------------------------------------------------------
// PP-2. 정상 흐름: kWaitClientResponse → RelayToServer
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ClientResponse_RelaysToServer) {
    const auto payload = make_payload(0x00, 50);  // HandshakeResponse
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitClientResponse,
        std::span<const std::uint8_t>{payload},
        0
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kWaitServerAuth);
    EXPECT_EQ(result->action, detail::HandshakeAction::kRelayToServer);
}

// ---------------------------------------------------------------------------
// PP-3. kWaitServerAuth + 0x00 → kDone + kComplete
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerAuth_OK_IsComplete) {
    const auto payload = make_payload(0x00);  // OK
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerAuth,
        std::span<const std::uint8_t>{payload},
        0
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kDone);
    EXPECT_EQ(result->action, detail::HandshakeAction::kComplete);
}

// ---------------------------------------------------------------------------
// PP-4. kWaitServerAuth + 0xFF → kFailed + kTerminate
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerAuth_ERR_IsTerminate) {
    const auto payload = make_payload(0xFF, 3);  // ERR
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerAuth,
        std::span<const std::uint8_t>{payload},
        0
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kFailed);
    EXPECT_EQ(result->action, detail::HandshakeAction::kTerminate);
}

// ---------------------------------------------------------------------------
// PP-5. kWaitServerAuth + 0xFE + < 9 bytes → kFailed + kTerminate (EOF)
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerAuth_EOF_IsTerminate) {
    const auto payload = make_payload(0xFE, 5);  // EOF (payload < 9)
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerAuth,
        std::span<const std::uint8_t>{payload},
        0
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kFailed);
    EXPECT_EQ(result->action, detail::HandshakeAction::kTerminate);
}

// ---------------------------------------------------------------------------
// PP-6. kWaitServerAuth + 0xFE + >= 9 bytes → kWaitClientAuthSwitch + RelayToClient
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerAuth_AuthSwitch_RelaysToClient) {
    const auto payload = make_payload(0xFE, 20);  // AuthSwitchRequest (payload >= 9)
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerAuth,
        std::span<const std::uint8_t>{payload},
        0
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kWaitClientAuthSwitch);
    EXPECT_EQ(result->action, detail::HandshakeAction::kRelayToClient);
}

// ---------------------------------------------------------------------------
// PP-7. kWaitServerAuth + 0x01 → kWaitClientMoreData + RelayToClient
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerAuth_AuthMoreData_RelaysToClient) {
    const auto payload = make_payload(0x01, 5);  // AuthMoreData
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerAuth,
        std::span<const std::uint8_t>{payload},
        0
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kWaitClientMoreData);
    EXPECT_EQ(result->action, detail::HandshakeAction::kRelayToClient);
}

// ---------------------------------------------------------------------------
// PP-8. kWaitServerAuth + unknown → kFailed + kTerminateNoRelay
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerAuth_Unknown_TerminatesNoRelay) {
    const auto payload = make_payload(0xAB, 2);  // unknown
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerAuth,
        std::span<const std::uint8_t>{payload},
        0
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kFailed);
    EXPECT_EQ(result->action, detail::HandshakeAction::kTerminateNoRelay);
}

// ---------------------------------------------------------------------------
// PP-9. AuthSwitch 흐름:
//   kWaitClientAuthSwitch → RelayToServer → kWaitServerAuthSwitch
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ClientAuthSwitch_RelaysToServer) {
    const auto payload = make_payload(0xAA, 10);  // client auth switch response
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitClientAuthSwitch,
        std::span<const std::uint8_t>{payload},
        1
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kWaitServerAuthSwitch);
    EXPECT_EQ(result->action, detail::HandshakeAction::kRelayToServer);
}

// ---------------------------------------------------------------------------
// PP-10. kWaitServerAuthSwitch + 0x00 → kDone + kComplete
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerAuthSwitch_OK_IsComplete) {
    const auto payload = make_payload(0x00);  // OK
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerAuthSwitch,
        std::span<const std::uint8_t>{payload},
        1
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kDone);
    EXPECT_EQ(result->action, detail::HandshakeAction::kComplete);
}

// ---------------------------------------------------------------------------
// PP-11. kWaitServerAuthSwitch + 0xFF → kFailed + kTerminate
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerAuthSwitch_ERR_IsTerminate) {
    const auto payload = make_payload(0xFF, 3);  // ERR
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerAuthSwitch,
        std::span<const std::uint8_t>{payload},
        1
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kFailed);
    EXPECT_EQ(result->action, detail::HandshakeAction::kTerminate);
}

// ---------------------------------------------------------------------------
// PP-12. AuthSwitch 후 AuthMoreData 체인:
//   kWaitServerAuthSwitch + 0x01 → kWaitClientMoreData + RelayToClient
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerAuthSwitch_AuthMoreData_Chains) {
    const auto payload = make_payload(0x01, 5);  // AuthMoreData
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerAuthSwitch,
        std::span<const std::uint8_t>{payload},
        1
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kWaitClientMoreData);
    EXPECT_EQ(result->action, detail::HandshakeAction::kRelayToClient);
}

// ---------------------------------------------------------------------------
// PP-13. AuthMoreData 흐름:
//   kWaitClientMoreData → RelayToServer → kWaitServerMoreData
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ClientMoreData_RelaysToServer) {
    const auto payload = make_payload(0xBB, 8);  // client more data
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitClientMoreData,
        std::span<const std::uint8_t>{payload},
        1
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kWaitServerMoreData);
    EXPECT_EQ(result->action, detail::HandshakeAction::kRelayToServer);
}

// ---------------------------------------------------------------------------
// PP-14. kWaitServerMoreData + 0x00 → kDone + kComplete
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerMoreData_OK_IsComplete) {
    const auto payload = make_payload(0x00);  // OK
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerMoreData,
        std::span<const std::uint8_t>{payload},
        1
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kDone);
    EXPECT_EQ(result->action, detail::HandshakeAction::kComplete);
}

// ---------------------------------------------------------------------------
// PP-15. kWaitServerMoreData + 0xFF → kFailed + kTerminate
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerMoreData_ERR_IsTerminate) {
    const auto payload = make_payload(0xFF, 3);  // ERR
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerMoreData,
        std::span<const std::uint8_t>{payload},
        1
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kFailed);
    EXPECT_EQ(result->action, detail::HandshakeAction::kTerminate);
}

// ---------------------------------------------------------------------------
// PP-16. kWaitServerMoreData + 0x01 → kWaitClientMoreData + RelayToClient
//         (AuthMoreData 반복 라운드트립)
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerMoreData_AuthMoreData_Repeats) {
    const auto payload = make_payload(0x01, 5);  // AuthMoreData
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerMoreData,
        std::span<const std::uint8_t>{payload},
        2  // round_trips=2, < kMaxRoundTrips(10)
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->next_state, detail::HandshakeState::kWaitClientMoreData);
    EXPECT_EQ(result->action, detail::HandshakeAction::kRelayToClient);
}

// ---------------------------------------------------------------------------
// PP-17. AuthMoreData 무한 루프 방지: round_trips >= kMaxRoundTrips → 에러
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerMoreData_MaxRoundTrips_IsError) {
    const auto payload = make_payload(0x01, 5);  // AuthMoreData
    // round_trips=10 == kMaxRoundTrips → 에러 반환
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerMoreData,
        std::span<const std::uint8_t>{payload},
        10  // == kMaxRoundTrips
    );
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
    EXPECT_FALSE(result.error().message.empty());
}

// ---------------------------------------------------------------------------
// PP-18. kWaitServerAuthSwitch + AuthMoreData + 무한 루프 방지
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerAuthSwitch_MaxRoundTrips_IsError) {
    const auto payload = make_payload(0x01, 5);  // AuthMoreData
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerAuthSwitch,
        std::span<const std::uint8_t>{payload},
        10  // == kMaxRoundTrips
    );
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// PP-19. AuthMoreData 중 AuthSwitch → fail-close (비정상)
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerMoreData_AuthSwitch_IsError) {
    const auto payload = make_payload(0xFE, 20);  // AuthSwitch (payload >= 9)
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerMoreData,
        std::span<const std::uint8_t>{payload},
        1
    );
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// PP-20. AuthSwitch 중 AuthSwitch 중첩 → fail-close
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerAuthSwitch_NestedAuthSwitch_IsError) {
    const auto payload = make_payload(0xFE, 20);  // AuthSwitch (payload >= 9)
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerAuthSwitch,
        std::span<const std::uint8_t>{payload},
        1
    );
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// PP-21. 종단 상태(kDone)에서 process_handshake_packet 호출 → 에러
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, TerminalState_Done_IsError) {
    const auto payload = make_payload(0x00);
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kDone,
        std::span<const std::uint8_t>{payload},
        0
    );
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kInternalError);
}

// ---------------------------------------------------------------------------
// PP-22. 종단 상태(kFailed)에서 process_handshake_packet 호출 → 에러
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, TerminalState_Failed_IsError) {
    const auto payload = make_payload(0xFF, 3);
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kFailed,
        std::span<const std::uint8_t>{payload},
        0
    );
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kInternalError);
}

// ---------------------------------------------------------------------------
// PP-23. kWaitServerGreeting + 빈 payload → 에러
// ---------------------------------------------------------------------------
TEST(ProcessHandshakePacket, ServerGreeting_EmptyPayload_IsError) {
    const std::vector<std::uint8_t> payload;
    const auto result = detail::process_handshake_packet(
        detail::HandshakeState::kWaitServerGreeting,
        std::span<const std::uint8_t>{payload},
        0
    );
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ===========================================================================
// Major 2: extract_handshake_response_fields — 강화된 검증 직접 테스트
// ===========================================================================

// 유효한 HandshakeResponse41 payload 빌더
// CLIENT_PROTOCOL_41 | CLIENT_SECURE_CONNECTION 기준
static std::vector<std::uint8_t> build_handshake_response(
    const std::string& username,
    const std::string& db_name,
    bool               with_db = true)
{
    // capability flags:
    //   CLIENT_LONG_PASSWORD(0x01) | CLIENT_PROTOCOL_41(0x0200) |
    //   CLIENT_SECURE_CONNECTION(0x8000)
    //   CLIENT_CONNECT_WITH_DB(0x08)은 with_db 파라미터에 따라 조건부 설정
    const std::uint32_t cap_flags = 0x00008201U | (with_db ? 0x00000008U : 0x00000000U);

    std::vector<std::uint8_t> payload;

    // capability flags (4바이트 LE)
    payload.push_back(static_cast<std::uint8_t>(cap_flags & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 8U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 16U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 24U) & 0xFF));

    // max_packet_size (4바이트)
    payload.push_back(0x00); payload.push_back(0x00);
    payload.push_back(0x00); payload.push_back(0x01);

    // charset (1바이트)
    payload.push_back(0x21);  // utf8

    // reserved (23바이트)
    for (int i = 0; i < 23; ++i) {
        payload.push_back(0x00);
    }
    // 여기까지 32바이트

    // username null-terminated
    for (char c : username) {
        payload.push_back(static_cast<std::uint8_t>(c));
    }
    payload.push_back(0x00);  // null terminator

    // auth_response (CLIENT_SECURE_CONNECTION: 1바이트 length + data)
    const std::string auth_data = "dummy_auth_data";
    payload.push_back(static_cast<std::uint8_t>(auth_data.size()));
    for (char c : auth_data) {
        payload.push_back(static_cast<std::uint8_t>(c));
    }

    // db_name null-terminated (if with_db)
    if (with_db) {
        for (char c : db_name) {
            payload.push_back(static_cast<std::uint8_t>(c));
        }
        payload.push_back(0x00);  // null terminator
    }

    return payload;
}

// ---------------------------------------------------------------------------
// EX-1. 정상 payload → db_user, db_name 정상 추출
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, Normal_ExtractsUserAndDb) {
    const auto payload = build_handshake_response("testuser", "testdb", true);

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(out_user, "testuser");
    EXPECT_EQ(out_db, "testdb");
}

// ---------------------------------------------------------------------------
// EX-2. payload 32바이트 미만 → ParseError (kMalformedPacket)
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, ShortPayload_IsError) {
    // 32바이트 payload (최소 33 미만)
    const std::vector<std::uint8_t> payload(32, 0x00);

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// EX-3. payload 정확히 0바이트 → ParseError
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, EmptyPayload_IsError) {
    const std::vector<std::uint8_t> payload;

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// EX-4. username null terminator 누락 → ParseError
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, UsernameNoNullTerminator_IsError) {
    // 최소 33바이트: 32바이트 고정 + username 1바이트(null terminator 없음)
    std::vector<std::uint8_t> payload(100, 0xAA);  // null terminator 없음
    // capability flags: CLIENT_SECURE_CONNECTION만 설정
    const std::uint32_t cap_flags = 0x00008000U;
    payload[0] = static_cast<std::uint8_t>(cap_flags & 0xFF);
    payload[1] = static_cast<std::uint8_t>((cap_flags >> 8U) & 0xFF);
    payload[2] = static_cast<std::uint8_t>((cap_flags >> 16U) & 0xFF);
    payload[3] = static_cast<std::uint8_t>((cap_flags >> 24U) & 0xFF);
    // username 영역(offset 32~): 0xAA로 가득 채워져 null이 없음

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// EX-5. auth_response lenenc 0xFE → ParseError
//        (CLIENT_PLUGIN_AUTH_LENENC 플래그 활성화 시)
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, AuthResponseLenencFE_IsError) {
    // CLIENT_PLUGIN_AUTH_LENENC(0x00200000) 활성화
    const std::uint32_t cap_flags = 0x00200000U;

    std::vector<std::uint8_t> payload;
    // capability flags
    payload.push_back(static_cast<std::uint8_t>(cap_flags & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 8U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 16U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 24U) & 0xFF));
    // max_packet_size + charset + reserved (28바이트)
    for (int i = 0; i < 28; ++i) { payload.push_back(0x00); }
    // username: "u\0"
    payload.push_back(static_cast<std::uint8_t>('u'));
    payload.push_back(0x00);
    // auth_response: 0xFE (잘못된 lenenc 변형)
    payload.push_back(0xFE);
    // 뒤에 8바이트 더미 (0xFE 뒤 파싱 전에 에러 반환)
    for (int i = 0; i < 8; ++i) { payload.push_back(0x00); }

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// EX-6. auth_response 길이 초과 → ParseError
//        (CLIENT_SECURE_CONNECTION: 1바이트 length 값 > 남은 바이트)
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, AuthResponseLengthExceedsPayload_IsError) {
    const std::uint32_t cap_flags = 0x00008000U;  // CLIENT_SECURE_CONNECTION

    std::vector<std::uint8_t> payload;
    payload.push_back(static_cast<std::uint8_t>(cap_flags & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 8U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 16U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 24U) & 0xFF));
    // max_packet_size + charset + reserved (28바이트)
    for (int i = 0; i < 28; ++i) { payload.push_back(0x00); }
    // username: "root\0"
    payload.push_back('r'); payload.push_back('o'); payload.push_back('o');
    payload.push_back('t'); payload.push_back(0x00);
    // auth_response: length=200, 실제 데이터는 5바이트만
    payload.push_back(200);  // 길이 200 선언
    payload.push_back(0xAA); payload.push_back(0xBB);
    payload.push_back(0xCC); payload.push_back(0xDD); payload.push_back(0xEE);
    // 실제 남은 바이트(5) < 선언 길이(200)

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// EX-7. CLIENT_CONNECT_WITH_DB 미설정 시 db_name 빈 문자열
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, NoConnectWithDb_DbNameIsEmpty) {
    // with_db=false → cap_flags에 CLIENT_CONNECT_WITH_DB 없음
    const auto payload = build_handshake_response("alice", "shouldbeignored", false);

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(out_user, "alice");
    EXPECT_TRUE(out_db.empty());
}

// ---------------------------------------------------------------------------
// EX-8. db_name null terminator 누락 → ParseError
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, DbNameNoNullTerminator_IsError) {
    // CLIENT_CONNECT_WITH_DB(0x08) + CLIENT_SECURE_CONNECTION(0x8000)
    const std::uint32_t cap_flags = 0x00008008U;

    std::vector<std::uint8_t> payload;
    payload.push_back(static_cast<std::uint8_t>(cap_flags & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 8U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 16U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 24U) & 0xFF));
    // max_packet_size + charset + reserved (28바이트)
    for (int i = 0; i < 28; ++i) { payload.push_back(0x00); }
    // username: "u\0"
    payload.push_back('u'); payload.push_back(0x00);
    // auth_response (CLIENT_SECURE_CONNECTION): length=1, data=0xAA
    payload.push_back(0x01); payload.push_back(0xAA);
    // db_name: "mydb" without null terminator
    payload.push_back('m'); payload.push_back('y');
    payload.push_back('d'); payload.push_back('b');
    // null terminator 없음 — 에러 발생해야 함

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// EX-9. 빈 username + db_name 정상 추출
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, EmptyUsername_IsOk) {
    // with_db=true, username=""
    const auto payload = build_handshake_response("", "mydb", true);

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(out_user.empty());
    EXPECT_EQ(out_db, "mydb");
}

// ---------------------------------------------------------------------------
// EX-10. auth_response lenenc 0xFC (2바이트 length) 정상 처리
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, AuthResponseLenencFC_Normal) {
    // CLIENT_PLUGIN_AUTH_LENENC(0x00200000)
    const std::uint32_t cap_flags = 0x00200000U;

    std::vector<std::uint8_t> payload;
    payload.push_back(static_cast<std::uint8_t>(cap_flags & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 8U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 16U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 24U) & 0xFF));
    for (int i = 0; i < 28; ++i) { payload.push_back(0x00); }
    // username: "bob\0"
    payload.push_back('b'); payload.push_back('o');
    payload.push_back('b'); payload.push_back(0x00);
    // auth_response: 0xFC + 2바이트 LE length=3, 3바이트 data
    payload.push_back(0xFC);
    payload.push_back(0x03); payload.push_back(0x00);  // length=3
    payload.push_back(0x11); payload.push_back(0x22); payload.push_back(0x33);
    // (CLIENT_CONNECT_WITH_DB 없으므로 db_name 파싱 안 함)

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(out_user, "bob");
    EXPECT_TRUE(out_db.empty());
}

// ===========================================================================
// Major 3: serialize() 상한 검증, make_error() truncation
// ===========================================================================

// ---------------------------------------------------------------------------
// T3-1. serialize(): 정상 패킷(payload <= 0xFFFFFF) → 비어있지 않은 벡터 반환
// ---------------------------------------------------------------------------
TEST(MysqlPacketSerialize, NormalPayloadSize) {
    // payload = {0x03, 0x41} (COM_QUERY + "A"), seq=0
    std::vector<std::uint8_t> data = {
        0x02, 0x00, 0x00,  // length = 2
        0x00,              // seq = 0
        0x03, 0x41
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());

    const auto serialized = result->serialize();
    // 정상 케이스: 비어있지 않아야 한다
    EXPECT_FALSE(serialized.empty());
    EXPECT_EQ(serialized.size(), 6U);  // 4(header) + 2(payload)
}

// ---------------------------------------------------------------------------
// T3-2. make_error(): 짧은 message → 그대로 포함 (truncation 없음)
// ---------------------------------------------------------------------------
TEST(MysqlPacketMakeError, ShortMessageNotTruncated) {
    const std::string short_msg = "short error";
    MysqlPacket pkt = MysqlPacket::make_error(1000, short_msg, 1);

    const auto payload = pkt.payload();
    // 9(fixed) + short_msg.size() = 전체 payload
    ASSERT_EQ(payload.size(), 9U + short_msg.size());

    // message 부분이 원본과 일치해야 한다
    const std::string decoded_msg(
        reinterpret_cast<const char*>(payload.data() + 9),
        payload.size() - 9
    );
    EXPECT_EQ(decoded_msg, short_msg);
}

// ---------------------------------------------------------------------------
// T3-3. make_error(): 최대 허용 길이의 message → 그대로 (경계값)
// ---------------------------------------------------------------------------
TEST(MysqlPacketMakeError, MaxLengthMessage) {
    // kMaxPayloadLen(0xFFFFFF) - kFixedHeaderLen(9) = 16776606 바이트
    static constexpr std::size_t kMaxMsg = 0x00FFFFFFU - 9U;
    const std::string max_msg(kMaxMsg, 'X');

    MysqlPacket pkt = MysqlPacket::make_error(9999, max_msg, 0);

    const auto payload = pkt.payload();
    ASSERT_EQ(payload.size(), 0x00FFFFFFU);  // 정확히 0xFFFFFF
    EXPECT_EQ(payload[0], 0xFF);

    // 메시지 시작 바이트 검증 (전체 복사는 너무 크므로 일부만)
    EXPECT_EQ(payload[9], static_cast<std::uint8_t>('X'));
}

// ---------------------------------------------------------------------------
// T3-4. make_error(): 상한 초과 message → truncation 처리 후 payload <= 0xFFFFFF
// ---------------------------------------------------------------------------
TEST(MysqlPacketMakeError, OversizedMessageTruncated) {
    // 0xFFFFFF - 9 + 1 = 상한을 1바이트 초과하는 message
    static constexpr std::size_t kOverMsg = 0x00FFFFFFU - 9U + 1U;
    const std::string over_msg(kOverMsg, 'Y');

    MysqlPacket pkt = MysqlPacket::make_error(1234, over_msg, 1);

    const auto payload = pkt.payload();
    // truncation 후 payload 크기는 정확히 0xFFFFFF
    ASSERT_EQ(payload.size(), 0x00FFFFFFU);

    // 첫 바이트는 여전히 0xFF (ERR marker)
    EXPECT_EQ(payload[0], 0xFF);

    // 메시지 시작 바이트는 'Y'
    EXPECT_EQ(payload[9], static_cast<std::uint8_t>('Y'));

    // serialize()도 빈 벡터가 아니어야 한다
    const auto serialized = pkt.serialize();
    EXPECT_FALSE(serialized.empty());
}

// ---------------------------------------------------------------------------
// T3-5. make_error(): 빈 message + truncation 로직 — 기존 동작 유지
// ---------------------------------------------------------------------------
TEST(MysqlPacketMakeError, EmptyMessageStillWorks) {
    MysqlPacket pkt = MysqlPacket::make_error(2000, "", 0);
    const auto payload = pkt.payload();
    ASSERT_EQ(payload.size(), 9U);  // 고정 9바이트
    EXPECT_EQ(payload[0], 0xFF);
}

// ---------------------------------------------------------------------------
// T3-6. serialize(): 정상 패킷 → 헤더+payload 구조 일관성
// ---------------------------------------------------------------------------
TEST(MysqlPacketSerialize, HeaderPayloadConsistency) {
    // payload가 0x03 + "ABC" (총 4바이트)
    std::vector<std::uint8_t> raw = {
        0x04, 0x00, 0x00,
        0x07,
        0x03, 0x41, 0x42, 0x43
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{raw});
    ASSERT_TRUE(result.has_value());

    const auto serialized = result->serialize();
    ASSERT_EQ(serialized.size(), 8U);

    // 헤더에서 길이 추출: 3바이트 LE
    const std::uint32_t hdr_len =
        static_cast<std::uint32_t>(serialized[0])
        | (static_cast<std::uint32_t>(serialized[1]) << 8U)
        | (static_cast<std::uint32_t>(serialized[2]) << 16U);
    EXPECT_EQ(hdr_len, 4U);

    // seq_id
    EXPECT_EQ(serialized[3], 0x07);
}

// ===========================================================================
// 기존 Major 1 간접 테스트 (PacketType 기반, 호환성 유지)
// ===========================================================================

// ---------------------------------------------------------------------------
// T1-1. 0xFE + payload.size() < 9 → kEof (EOF, 핸드셰이크 실패 경로)
// ---------------------------------------------------------------------------
TEST(AuthResponseBranch, FE_SmallPayload_IsEof) {
    // payload = {0xFE, 0x00, 0x00, 0x02, 0x00} (5바이트, < 9)
    std::vector<std::uint8_t> data = {
        0x05, 0x00, 0x00,
        0x02,
        0xFE, 0x00, 0x00, 0x02, 0x00
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kEof);

    const auto payload = result->payload();
    ASSERT_FALSE(payload.empty());
    EXPECT_EQ(payload[0], 0xFE);
    EXPECT_LT(payload.size(), 9U);
}

// ---------------------------------------------------------------------------
// T1-2. 0xFE + payload.size() >= 9 → kUnknown (AuthSwitchRequest 분기)
// ---------------------------------------------------------------------------
TEST(AuthResponseBranch, FE_LargePayload_IsAuthSwitchRequest) {
    // payload = 0xFE + 9바이트 more (총 10바이트 payload)
    std::vector<std::uint8_t> payload_body = {
        0xFE,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09
    };
    const std::uint32_t len = static_cast<std::uint32_t>(payload_body.size());

    std::vector<std::uint8_t> data;
    data.push_back(static_cast<std::uint8_t>(len & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 8U) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 16U) & 0xFF));
    data.push_back(0x03);  // seq=3
    data.insert(data.end(), payload_body.begin(), payload_body.end());

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kUnknown);

    const auto payload = result->payload();
    EXPECT_EQ(payload[0], 0xFE);
    EXPECT_GE(payload.size(), 9U);
}

// ---------------------------------------------------------------------------
// T1-3. 0x01 → AuthMoreData 마커
// ---------------------------------------------------------------------------
TEST(AuthResponseBranch, X01_AuthMoreData_Marker) {
    std::vector<std::uint8_t> data = {
        0x05, 0x00, 0x00,
        0x03,
        0x01, 0xAA, 0xBB, 0xCC, 0xDD
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());

    const auto payload = result->payload();
    ASSERT_FALSE(payload.empty());
    EXPECT_EQ(payload[0], 0x01);
}

// ---------------------------------------------------------------------------
// T1-4. 0x00 → kOk
// ---------------------------------------------------------------------------
TEST(AuthResponseBranch, X00_IsOk) {
    std::vector<std::uint8_t> data = {
        0x01, 0x00, 0x00,
        0x02,
        0x00
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kOk);

    const auto payload = result->payload();
    EXPECT_EQ(payload[0], 0x00);
}

// ---------------------------------------------------------------------------
// T1-5. 0xFF → kError
// ---------------------------------------------------------------------------
TEST(AuthResponseBranch, XFF_IsError) {
    std::vector<std::uint8_t> data = {
        0x03, 0x00, 0x00,
        0x02,
        0xFF, 0x15, 0x04
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kError);

    const auto payload = result->payload();
    EXPECT_EQ(payload[0], 0xFF);
}

// ---------------------------------------------------------------------------
// T1-6. unknown type (0xAB) → kUnknown (else 분기: fail-close)
// ---------------------------------------------------------------------------
TEST(AuthResponseBranch, UnknownType_IsUnknown) {
    std::vector<std::uint8_t> data = {
        0x02, 0x00, 0x00,
        0x03,
        0xAB, 0x01
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kUnknown);

    const auto payload = result->payload();
    EXPECT_EQ(payload[0], 0xAB);
}

// ---------------------------------------------------------------------------
// T1-7. 빈 payload → auth 응답에서 빈 payload 감지 경로
// ---------------------------------------------------------------------------
TEST(AuthResponseBranch, EmptyPayload_IsUnknown) {
    std::vector<std::uint8_t> data = {
        0x00, 0x00, 0x00,
        0x02
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kUnknown);
    EXPECT_TRUE(result->payload().empty());
}

// ---------------------------------------------------------------------------
// T1-8. 0xFE + 정확히 payload.size() == 8 → kEof (경계값)
// ---------------------------------------------------------------------------
TEST(AuthResponseBranch, FE_Exactly8Bytes_IsEof) {
    std::vector<std::uint8_t> data = {
        0x08, 0x00, 0x00,
        0x01,
        0xFE, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kEof);

    const auto payload = result->payload();
    EXPECT_EQ(payload.size(), 8U);
}

// ---------------------------------------------------------------------------
// T1-9. 0xFE + 정확히 payload.size() == 9 → kUnknown (경계값)
// ---------------------------------------------------------------------------
TEST(AuthResponseBranch, FE_Exactly9Bytes_IsAuthSwitchRequest) {
    std::vector<std::uint8_t> data = {
        0x09, 0x00, 0x00,
        0x01,
        0xFE, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kUnknown);

    const auto payload = result->payload();
    EXPECT_EQ(payload.size(), 9U);
}

// ===========================================================================
// 기존 Major 2 간접 테스트 (호환성 유지)
// ===========================================================================

TEST(HandshakeResponseFields, ShortPayloadIsDetectedAsSmall) {
    std::vector<std::uint8_t> payload_32(32, 0x00);
    payload_32[0] = 0x08;

    std::vector<std::uint8_t> raw;
    raw.push_back(0x20);
    raw.push_back(0x00);
    raw.push_back(0x00);
    raw.push_back(0x01);
    raw.insert(raw.end(), payload_32.begin(), payload_32.end());

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{raw});
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->payload_length(), 32U);
    EXPECT_LT(result->payload().size(), 33U);
}

TEST(HandshakeResponseFields, SufficientPayloadSize) {
    std::vector<std::uint8_t> payload_33(33, 0x00);
    payload_33[0] = 0x00;

    std::vector<std::uint8_t> raw;
    raw.push_back(0x21);
    raw.push_back(0x00);
    raw.push_back(0x00);
    raw.push_back(0x01);
    raw.insert(raw.end(), payload_33.begin(), payload_33.end());

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{raw});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_length(), 33U);
    EXPECT_GE(result->payload().size(), 33U);
}

// ===========================================================================
// fail-open 수정 검증 (DON-24 fix)
// ===========================================================================

// ---------------------------------------------------------------------------
// FO-1. CLIENT_SECURE_CONNECTION: auth_response 길이 prefix 자체 누락
//        (payload가 username null-terminator 직후에서 잘림)
//        → ParseError(kMalformedPacket) 반환 (fail-close)
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, SecureConn_AuthLenPrefixMissing_IsError) {
    // CLIENT_SECURE_CONNECTION(0x00008000) 설정
    // CLIENT_CONNECT_WITH_DB 미설정
    const std::uint32_t cap_flags = 0x00008000U;

    std::vector<std::uint8_t> payload;
    // capability flags (4바이트 LE)
    payload.push_back(static_cast<std::uint8_t>(cap_flags & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 8U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 16U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 24U) & 0xFF));
    // max_packet_size (4바이트) + charset (1바이트) + reserved (23바이트) = 28바이트
    for (int i = 0; i < 28; ++i) { payload.push_back(0x00); }
    // 여기까지 32바이트
    // username: "alice\0"
    payload.push_back(static_cast<std::uint8_t>('a'));
    payload.push_back(static_cast<std::uint8_t>('l'));
    payload.push_back(static_cast<std::uint8_t>('i'));
    payload.push_back(static_cast<std::uint8_t>('c'));
    payload.push_back(static_cast<std::uint8_t>('e'));
    payload.push_back(0x00);  // null terminator
    // auth_response 길이 prefix 없이 payload 끝 (pos >= payload.size())

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
    // 에러 메시지에 "auth_response length prefix missing" 포함 확인
    EXPECT_NE(result.error().message.find("auth_response length prefix missing"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// FO-2. CLIENT_CONNECT_WITH_DB 설정 + db_name 필드 전체 누락
//        (auth_response 이후 payload가 즉시 끝남)
//        → ParseError(kMalformedPacket) 반환 (fail-close)
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, ConnectWithDb_DbFieldMissing_IsError) {
    // CLIENT_CONNECT_WITH_DB(0x00000008) + CLIENT_SECURE_CONNECTION(0x00008000)
    const std::uint32_t cap_flags = 0x00008008U;

    std::vector<std::uint8_t> payload;
    // capability flags (4바이트 LE)
    payload.push_back(static_cast<std::uint8_t>(cap_flags & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 8U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 16U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 24U) & 0xFF));
    // max_packet_size (4바이트) + charset (1바이트) + reserved (23바이트) = 28바이트
    for (int i = 0; i < 28; ++i) { payload.push_back(0x00); }
    // 여기까지 32바이트
    // username: "root\0"
    payload.push_back(static_cast<std::uint8_t>('r'));
    payload.push_back(static_cast<std::uint8_t>('o'));
    payload.push_back(static_cast<std::uint8_t>('o'));
    payload.push_back(static_cast<std::uint8_t>('t'));
    payload.push_back(0x00);  // null terminator
    // auth_response (CLIENT_SECURE_CONNECTION): length=3, 3바이트 데이터
    payload.push_back(0x03);  // length prefix
    payload.push_back(0xAA);
    payload.push_back(0xBB);
    payload.push_back(0xCC);
    // db_name 필드 전체 누락 — payload 종료 (pos >= payload.size())

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
    // 에러 메시지에 "database field missing" 포함 확인
    EXPECT_NE(result.error().message.find("database field missing"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// FO-3. legacy auth_response(null-terminated) terminator 누락
//        (CLIENT_SECURE_CONNECTION / CLIENT_PLUGIN_AUTH_LENENC 모두 미설정)
//        → ParseError(kMalformedPacket) 반환 (fail-close)
// ---------------------------------------------------------------------------
TEST(ExtractHandshakeResponseFields, LegacyAuthResponseNoNullTerminator_IsError) {
    const std::uint32_t cap_flags = 0x00000000U;

    std::vector<std::uint8_t> payload;
    // capability flags (4바이트 LE)
    payload.push_back(static_cast<std::uint8_t>(cap_flags & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 8U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 16U) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((cap_flags >> 24U) & 0xFF));
    // max_packet_size (4바이트) + charset (1바이트) + reserved (23바이트) = 28바이트
    for (int i = 0; i < 28; ++i) { payload.push_back(0x00); }
    // username: "u\0"
    payload.push_back(static_cast<std::uint8_t>('u'));
    payload.push_back(0x00);
    // legacy auth_response: "abc" (null terminator 누락)
    payload.push_back(static_cast<std::uint8_t>('a'));
    payload.push_back(static_cast<std::uint8_t>('b'));
    payload.push_back(static_cast<std::uint8_t>('c'));

    std::string out_user;
    std::string out_db;
    const auto result = detail::extract_handshake_response_fields(
        std::span<const std::uint8_t>{payload},
        out_user,
        out_db
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
    EXPECT_NE(result.error().message.find("auth_response missing null terminator"),
              std::string::npos);
}
