// ---------------------------------------------------------------------------
// test_mysql_packet.cpp
//
// MysqlPacket / extract_command 단위 테스트 (DON-24)
// ---------------------------------------------------------------------------

#include "protocol/command.hpp"
#include "protocol/mysql_packet.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

// ===========================================================================
// MysqlPacket::parse 테스트
// ===========================================================================

// ---------------------------------------------------------------------------
// 1. parse 정상: 4바이트 헤더 + N바이트 payload
// ---------------------------------------------------------------------------
TEST(MysqlPacketParse, SuccessNormalPacket) {
    // payload = {0x03, 0x53, 0x45, 0x4C} ("SELECT" 앞 4글자)
    // length = 4, seq_id = 0
    std::vector<std::uint8_t> data = {
        0x04, 0x00, 0x00,  // payload length = 4 (LE)
        0x00,              // sequence_id = 0
        0x03, 0x53, 0x45, 0x4C  // payload: 0x03 = COM_QUERY, "SEL"
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());

    const MysqlPacket& pkt = *result;
    EXPECT_EQ(pkt.sequence_id(), 0);
    EXPECT_EQ(pkt.payload_length(), 4U);
    EXPECT_EQ(pkt.type(), PacketType::kComQuery);
}

// ---------------------------------------------------------------------------
// 2. parse 에러: 3바이트 미만 → ParseError (kMalformedPacket)
// ---------------------------------------------------------------------------
TEST(MysqlPacketParse, ErrorTooShort) {
    std::vector<std::uint8_t> data = {0x01, 0x00};  // 2바이트만

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
    EXPECT_FALSE(result.error().message.empty());
}

// ---------------------------------------------------------------------------
// 3. parse 에러: declared length > 실제 데이터 → ParseError
// ---------------------------------------------------------------------------
TEST(MysqlPacketParse, ErrorIncompletePayload) {
    // length=10 인데 실제 payload는 3바이트만
    std::vector<std::uint8_t> data = {
        0x0A, 0x00, 0x00,  // payload length = 10
        0x01,              // sequence_id = 1
        0x03, 0x41, 0x42   // payload 3바이트만 제공
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// 4. parse 빈 payload: length=0 → 성공 (COM_QUIT용)
// ---------------------------------------------------------------------------
TEST(MysqlPacketParse, SuccessEmptyPayload) {
    std::vector<std::uint8_t> data = {
        0x00, 0x00, 0x00,  // payload length = 0
        0x00               // sequence_id = 0
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());

    const MysqlPacket& pkt = *result;
    EXPECT_EQ(pkt.payload_length(), 0U);
    EXPECT_EQ(pkt.type(), PacketType::kUnknown);
    EXPECT_TRUE(pkt.payload().empty());
}

// ===========================================================================
// PacketType 판별 테스트
// ===========================================================================

// ---------------------------------------------------------------------------
// 5-a. PacketType 판별: 0xFF → kError
// ---------------------------------------------------------------------------
TEST(MysqlPacketType, ErrorPacket) {
    std::vector<std::uint8_t> data = {
        0x03, 0x00, 0x00,  // length = 3
        0x00,              // seq = 0
        0xFF, 0x01, 0x02   // ERR marker + 2바이트 더미
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kError);
}

// ---------------------------------------------------------------------------
// 5-b. PacketType 판별: 0x0A → kHandshake
// ---------------------------------------------------------------------------
TEST(MysqlPacketType, HandshakePacket) {
    std::vector<std::uint8_t> data = {
        0x02, 0x00, 0x00,  // length = 2
        0x00,              // seq = 0
        0x0A, 0x41         // Handshake marker + 1바이트 더미
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kHandshake);
}

// ---------------------------------------------------------------------------
// 5-c. PacketType 판별: 0x03 → kComQuery
// ---------------------------------------------------------------------------
TEST(MysqlPacketType, ComQueryPacket) {
    std::vector<std::uint8_t> data = {
        0x04, 0x00, 0x00,  // length = 4
        0x00,              // seq = 0
        0x03, 0x53, 0x45, 0x4C  // COM_QUERY + "SEL"
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kComQuery);
}

// ---------------------------------------------------------------------------
// 5-d. PacketType 판별: 0xFE (payload < 9바이트) → kEof
// ---------------------------------------------------------------------------
TEST(MysqlPacketType, EofPacket) {
    // EOF packet: 0xFE + 4바이트 (총 5바이트 payload, < 9)
    std::vector<std::uint8_t> data = {
        0x05, 0x00, 0x00,
        0x01,
        0xFE, 0x00, 0x00, 0x02, 0x00
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kEof);
}

// ---------------------------------------------------------------------------
// 5-e. PacketType 판별: 0x00 → kOk
// ---------------------------------------------------------------------------
TEST(MysqlPacketType, OkPacket) {
    std::vector<std::uint8_t> data = {
        0x01, 0x00, 0x00,
        0x02,
        0x00
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), PacketType::kOk);
}

// ===========================================================================
// make_error 테스트
// ===========================================================================

// ---------------------------------------------------------------------------
// 6. make_error: ERR Packet 포맷 검증
// ---------------------------------------------------------------------------
TEST(MysqlPacketMakeError, ErrorPacketFormat) {
    const std::uint16_t error_code = 1045U;
    const std::string   message    = "Access denied";
    const std::uint8_t  seq_id     = 2;

    MysqlPacket pkt = MysqlPacket::make_error(error_code, message, seq_id);

    EXPECT_EQ(pkt.sequence_id(), seq_id);
    EXPECT_EQ(pkt.type(), PacketType::kError);

    const auto payload = pkt.payload();
    // 최소 길이: 1(0xFF) + 2(error_code) + 1('#') + 5(sql_state) + message.size()
    ASSERT_GE(payload.size(), 1U + 2U + 1U + 5U + message.size());

    // 첫 바이트: 0xFF
    EXPECT_EQ(payload[0], 0xFF);

    // error_code (LE)
    const std::uint16_t decoded_code =
        static_cast<std::uint16_t>(payload[1])
        | (static_cast<std::uint16_t>(payload[2]) << 8U);
    EXPECT_EQ(decoded_code, error_code);

    // '#' marker
    EXPECT_EQ(payload[3], static_cast<std::uint8_t>('#'));

    // sql_state: "HY000"
    EXPECT_EQ(payload[4], static_cast<std::uint8_t>('H'));
    EXPECT_EQ(payload[5], static_cast<std::uint8_t>('Y'));
    EXPECT_EQ(payload[6], static_cast<std::uint8_t>('0'));
    EXPECT_EQ(payload[7], static_cast<std::uint8_t>('0'));
    EXPECT_EQ(payload[8], static_cast<std::uint8_t>('0'));

    // message
    const std::string decoded_msg(
        reinterpret_cast<const char*>(payload.data() + 9),
        payload.size() - 9
    );
    EXPECT_EQ(decoded_msg, message);
}

// ===========================================================================
// serialize 테스트
// ===========================================================================

// ---------------------------------------------------------------------------
// 7. serialize: 헤더 3바이트 LE + seq_id + payload
// ---------------------------------------------------------------------------
TEST(MysqlPacketSerialize, RoundTrip) {
    // 원본 패킷 데이터
    std::vector<std::uint8_t> original = {
        0x05, 0x00, 0x00,  // length = 5
        0x03,              // seq = 3
        0x03, 0x41, 0x42, 0x43, 0x44  // COM_QUERY + "ABCD"
    };

    auto parse_result = MysqlPacket::parse(std::span<const std::uint8_t>{original});
    ASSERT_TRUE(parse_result.has_value());

    const auto serialized = parse_result->serialize();

    ASSERT_EQ(serialized.size(), original.size());
    EXPECT_EQ(serialized, original);
}

TEST(MysqlPacketSerialize, HeaderFormat) {
    // payload = {0xFF, 0x01, 0x02} (ERR 패킷), seq=1
    std::vector<std::uint8_t> data = {
        0x03, 0x00, 0x00,
        0x01,
        0xFF, 0x01, 0x02
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());

    const auto serialized = result->serialize();
    ASSERT_GE(serialized.size(), 4U);

    // 3바이트 LE length
    const std::uint32_t len =
        static_cast<std::uint32_t>(serialized[0])
        | (static_cast<std::uint32_t>(serialized[1]) << 8U)
        | (static_cast<std::uint32_t>(serialized[2]) << 16U);
    EXPECT_EQ(len, 3U);

    // sequence_id
    EXPECT_EQ(serialized[3], 0x01);
}

// ===========================================================================
// extract_command 테스트
// ===========================================================================

// ---------------------------------------------------------------------------
// 8. extract_command: COM_QUERY → query 추출
// ---------------------------------------------------------------------------
TEST(ExtractCommand, ComQueryExtractsQuery) {
    const std::string sql       = "SELECT 1";
    const std::uint8_t seq_id   = 0;

    // payload: {0x03} + sql bytes
    std::vector<std::uint8_t> payload_bytes;
    payload_bytes.push_back(0x03);
    for (char c : sql) {
        payload_bytes.push_back(static_cast<std::uint8_t>(c));
    }

    // wire format
    const std::uint32_t len = static_cast<std::uint32_t>(payload_bytes.size());
    std::vector<std::uint8_t> data;
    data.push_back(static_cast<std::uint8_t>(len & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    data.push_back(seq_id);
    data.insert(data.end(), payload_bytes.begin(), payload_bytes.end());

    auto parse_result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(parse_result.has_value());

    auto cmd_result = extract_command(*parse_result);
    ASSERT_TRUE(cmd_result.has_value());

    EXPECT_EQ(cmd_result->command_type, CommandType::kComQuery);
    EXPECT_EQ(cmd_result->query, sql);
    EXPECT_EQ(cmd_result->sequence_id, seq_id);
}

// ---------------------------------------------------------------------------
// 9. extract_command: COM_QUIT → CommandType::kComQuit
// ---------------------------------------------------------------------------
TEST(ExtractCommand, ComQuit) {
    std::vector<std::uint8_t> data = {
        0x01, 0x00, 0x00,  // length = 1
        0x00,              // seq = 0
        0x01               // COM_QUIT
    };

    auto parse_result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(parse_result.has_value());

    auto cmd_result = extract_command(*parse_result);
    ASSERT_TRUE(cmd_result.has_value());

    EXPECT_EQ(cmd_result->command_type, CommandType::kComQuit);
    EXPECT_TRUE(cmd_result->query.empty());
}

// ---------------------------------------------------------------------------
// 10. extract_command: 빈 payload → kMalformedPacket
// ---------------------------------------------------------------------------
TEST(ExtractCommand, EmptyPayload) {
    std::vector<std::uint8_t> data = {
        0x00, 0x00, 0x00,  // length = 0
        0x00               // seq = 0
    };

    auto parse_result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(parse_result.has_value());

    auto cmd_result = extract_command(*parse_result);
    ASSERT_FALSE(cmd_result.has_value());
    EXPECT_EQ(cmd_result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// 11. extract_command: 미지원 커맨드 바이트(0xAA) → kUnsupportedCommand
// ---------------------------------------------------------------------------
TEST(ExtractCommand, UnsupportedCommand) {
    std::vector<std::uint8_t> data = {
        0x01, 0x00, 0x00,  // length = 1
        0x00,              // seq = 0
        0xAA               // 미지원 커맨드
    };

    auto parse_result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(parse_result.has_value());

    auto cmd_result = extract_command(*parse_result);
    ASSERT_FALSE(cmd_result.has_value());
    EXPECT_EQ(cmd_result.error().code, ParseErrorCode::kUnsupportedCommand);
    // 에러 메시지에 커맨드 바이트 값 포함 확인
    EXPECT_FALSE(cmd_result.error().message.empty());
}

// ===========================================================================
// 추가 경계 테스트
// ===========================================================================

// make_error로 생성한 패킷을 parse로 재파싱하면 kError 타입이어야 한다
TEST(MysqlPacketMakeError, ParsedBackAsError) {
    MysqlPacket err_pkt = MysqlPacket::make_error(1064, "syntax error", 1);
    const auto  serialized = err_pkt.serialize();

    auto re_parsed = MysqlPacket::parse(std::span<const std::uint8_t>{serialized});
    ASSERT_TRUE(re_parsed.has_value());
    EXPECT_EQ(re_parsed->type(), PacketType::kError);
    EXPECT_EQ(re_parsed->sequence_id(), 1);
}

// COM_QUERY with empty query body (payload = {0x03} only)
TEST(ExtractCommand, ComQueryEmptyQueryBody) {
    std::vector<std::uint8_t> data = {
        0x01, 0x00, 0x00,
        0x00,
        0x03  // COM_QUERY, body 없음
    };

    auto parse_result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(parse_result.has_value());

    auto cmd_result = extract_command(*parse_result);
    ASSERT_TRUE(cmd_result.has_value());
    EXPECT_EQ(cmd_result->command_type, CommandType::kComQuery);
    EXPECT_TRUE(cmd_result->query.empty());
}

// sequence_id가 헤더에 정확히 저장/복원되는지 검증
TEST(MysqlPacketParse, SequenceIdPreserved) {
    std::vector<std::uint8_t> data = {
        0x01, 0x00, 0x00,
        0x42,  // seq = 0x42
        0x0E   // COM_PING
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sequence_id(), 0x42);

    const auto serialized = result->serialize();
    EXPECT_EQ(serialized[3], 0x42);
}

// ===========================================================================
// DON-24 누락 케이스 보강: COM_INIT_DB, COM_STMT_PREPARE, 멀티바이트 SQL,
//                         0xFE large payload (non-EOF)
// ===========================================================================

// ---------------------------------------------------------------------------
// 12. extract_command: COM_INIT_DB (0x02) → kComInitDb
// ---------------------------------------------------------------------------
TEST(ExtractCommand, ComInitDb) {
    // 0x02 = COM_INIT_DB, payload = 0x02 + "testdb"
    const std::string db_name = "testdb";

    std::vector<std::uint8_t> payload_bytes;
    payload_bytes.push_back(0x02);  // COM_INIT_DB
    for (char c : db_name) {
        payload_bytes.push_back(static_cast<std::uint8_t>(c));
    }

    const std::uint32_t len = static_cast<std::uint32_t>(payload_bytes.size());
    std::vector<std::uint8_t> data;
    data.push_back(static_cast<std::uint8_t>(len & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    data.push_back(0x00);  // seq = 0
    data.insert(data.end(), payload_bytes.begin(), payload_bytes.end());

    auto parse_result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(parse_result.has_value());

    auto cmd_result = extract_command(*parse_result);
    ASSERT_TRUE(cmd_result.has_value());
    EXPECT_EQ(cmd_result->command_type, CommandType::kComInitDb);
    // COM_INIT_DB는 query 필드를 설정하지 않는다 (COM_QUERY 전용)
    EXPECT_TRUE(cmd_result->query.empty());
}

// ---------------------------------------------------------------------------
// 13. extract_command: COM_STMT_PREPARE (0x16) → kComStmtPrepare
// ---------------------------------------------------------------------------
TEST(ExtractCommand, ComStmtPrepare) {
    // 0x16 = COM_STMT_PREPARE, payload = 0x16 + "SELECT ? FROM users"
    const std::string stmt_sql = "SELECT ? FROM users";

    std::vector<std::uint8_t> payload_bytes;
    payload_bytes.push_back(0x16);  // COM_STMT_PREPARE
    for (char c : stmt_sql) {
        payload_bytes.push_back(static_cast<std::uint8_t>(c));
    }

    const std::uint32_t len = static_cast<std::uint32_t>(payload_bytes.size());
    std::vector<std::uint8_t> data;
    data.push_back(static_cast<std::uint8_t>(len & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    data.push_back(0x00);  // seq = 0
    data.insert(data.end(), payload_bytes.begin(), payload_bytes.end());

    auto parse_result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(parse_result.has_value());

    auto cmd_result = extract_command(*parse_result);
    ASSERT_TRUE(cmd_result.has_value());
    EXPECT_EQ(cmd_result->command_type, CommandType::kComStmtPrepare);
    // COM_STMT_PREPARE는 query 필드를 설정하지 않는다 (COM_QUERY 전용)
    EXPECT_TRUE(cmd_result->query.empty());
}

// ---------------------------------------------------------------------------
// 14. extract_command: COM_QUERY with multibyte UTF-8 characters
//     "SELECT '안녕'" — 한글은 UTF-8 인코딩에서 3바이트/문자
// ---------------------------------------------------------------------------
TEST(ExtractCommand, ComQueryMultibyteChars) {
    // UTF-8 encoded Korean: 안(0xEC 0x95 0x88) 녕(0xEB 0x85 0x95)
    // C++20+: u8 literal produces char8_t; use explicit byte string instead.
    const std::string multibyte_sql = "SELECT '\xEC\x95\x88\xEB\x85\x95'";

    std::vector<std::uint8_t> payload_bytes;
    payload_bytes.push_back(0x03);  // COM_QUERY
    for (char c : multibyte_sql) {
        payload_bytes.push_back(static_cast<std::uint8_t>(c));
    }

    const std::uint32_t len = static_cast<std::uint32_t>(payload_bytes.size());
    std::vector<std::uint8_t> data;
    data.push_back(static_cast<std::uint8_t>(len & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    data.push_back(0x00);  // seq = 0
    data.insert(data.end(), payload_bytes.begin(), payload_bytes.end());

    auto parse_result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(parse_result.has_value());

    auto cmd_result = extract_command(*parse_result);
    ASSERT_TRUE(cmd_result.has_value());
    EXPECT_EQ(cmd_result->command_type, CommandType::kComQuery);
    // 바이트 단위 비교: SQL 문자열은 그대로 보존되어야 한다
    EXPECT_EQ(cmd_result->query, multibyte_sql);
    // payload 내 멀티바이트 문자가 포함되어 있으므로 query.size() > 글자 수
    EXPECT_GT(cmd_result->query.size(), 8U);  // "SELECT '" (8chars) + 한글 2자(6bytes) + "'"
}

// ---------------------------------------------------------------------------
// 15. PacketType 판별: 0xFE + payload >= 9바이트 → kUnknown (EOF가 아님)
//     MySQL 프로토콜에서 0xFE는 payload < 9일 때만 진짜 EOF 패킷이다.
//     payload >= 9이면 다른 타입(구현에서 kUnknown으로 처리)이어야 한다.
// ---------------------------------------------------------------------------
TEST(MysqlPacketType, FELargePayloadNotEof) {
    // 0xFE + 9바이트 = 총 10바이트 payload (payload >= 9 → kUnknown)
    std::vector<std::uint8_t> payload_body = {
        0xFE,                               // first byte
        0x01, 0x02, 0x03, 0x04,             // 4바이트 more
        0x05, 0x06, 0x07, 0x08, 0x09        // 5바이트 more (total: 1+9 = 10 bytes payload)
    };
    const std::uint32_t len = static_cast<std::uint32_t>(payload_body.size());

    std::vector<std::uint8_t> data;
    data.push_back(static_cast<std::uint8_t>(len & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    data.push_back(0x00);  // seq = 0
    data.insert(data.end(), payload_body.begin(), payload_body.end());

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    // 0xFE이지만 payload.size() >= 9 이므로 kEof가 아닌 kUnknown이어야 한다
    EXPECT_EQ(result->type(), PacketType::kUnknown);
    EXPECT_NE(result->type(), PacketType::kEof);
}

// ---------------------------------------------------------------------------
// 16. sequence_id 오버플로우 경계값: 255 (0xFF) seq_id 보존 검증
// ---------------------------------------------------------------------------
TEST(MysqlPacketParse, SequenceIdMaxValue) {
    std::vector<std::uint8_t> data = {
        0x01, 0x00, 0x00,
        0xFF,  // seq = 255 (max uint8_t)
        0x0E   // COM_PING
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sequence_id(), 0xFF);

    // 직렬화 후 재파싱해도 255 보존
    const auto serialized = result->serialize();
    EXPECT_EQ(serialized[3], 0xFF);

    auto re_parsed = MysqlPacket::parse(std::span<const std::uint8_t>{serialized});
    ASSERT_TRUE(re_parsed.has_value());
    EXPECT_EQ(re_parsed->sequence_id(), 0xFF);
}

// ---------------------------------------------------------------------------
// 17. parse: 정확히 4바이트 (헤더만, payload 없음) → 성공
//     4바이트 미만만 실패해야 하며, 정확히 4바이트는 length=0인 경우 성공한다.
// ---------------------------------------------------------------------------
TEST(MysqlPacketParse, ExactlyFourBytes) {
    std::vector<std::uint8_t> data = {
        0x00, 0x00, 0x00,  // length = 0
        0x05               // seq = 5
    };

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_length(), 0U);
    EXPECT_EQ(result->sequence_id(), 5);
}

// ---------------------------------------------------------------------------
// 18. parse: 1바이트 패킷 → kMalformedPacket
// ---------------------------------------------------------------------------
TEST(MysqlPacketParse, OneByte) {
    std::vector<std::uint8_t> data = {0x01};

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
}

// ---------------------------------------------------------------------------
// 19. parse: 빈 스팬 → kMalformedPacket
// ---------------------------------------------------------------------------
TEST(MysqlPacketParse, EmptySpan) {
    std::vector<std::uint8_t> data;

    auto result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ParseErrorCode::kMalformedPacket);
    EXPECT_FALSE(result.error().message.empty());
}

// ---------------------------------------------------------------------------
// 20. extract_command: COM_PING (0x0E) → kComPing
//     단일 커맨드 바이트만 있는 패킷도 올바르게 분류되는지 검증
// ---------------------------------------------------------------------------
TEST(ExtractCommand, ComPing) {
    std::vector<std::uint8_t> data = {
        0x01, 0x00, 0x00,
        0x00,
        0x0E  // COM_PING
    };

    auto parse_result = MysqlPacket::parse(std::span<const std::uint8_t>{data});
    ASSERT_TRUE(parse_result.has_value());

    auto cmd_result = extract_command(*parse_result);
    ASSERT_TRUE(cmd_result.has_value());
    EXPECT_EQ(cmd_result->command_type, CommandType::kComPing);
    EXPECT_TRUE(cmd_result->query.empty());
}

// ---------------------------------------------------------------------------
// 21. make_error: sequence_id=0 경계값 검증
// ---------------------------------------------------------------------------
TEST(MysqlPacketMakeError, SequenceIdZero) {
    MysqlPacket pkt = MysqlPacket::make_error(2003, "Can't connect", 0);
    EXPECT_EQ(pkt.sequence_id(), 0);
    EXPECT_EQ(pkt.type(), PacketType::kError);

    const auto payload = pkt.payload();
    ASSERT_FALSE(payload.empty());
    EXPECT_EQ(payload[0], 0xFF);
}

// ---------------------------------------------------------------------------
// 22. make_error: 빈 메시지 문자열 처리 (메시지 없는 ERR 패킷)
// ---------------------------------------------------------------------------
TEST(MysqlPacketMakeError, EmptyMessage) {
    MysqlPacket pkt = MysqlPacket::make_error(1000, "", 1);
    EXPECT_EQ(pkt.type(), PacketType::kError);

    const auto payload = pkt.payload();
    // 최소 구조: 0xFF + 2바이트 코드 + '#' + 5바이트 sql_state = 9바이트
    ASSERT_EQ(payload.size(), 9U);
    EXPECT_EQ(payload[0], 0xFF);
}

