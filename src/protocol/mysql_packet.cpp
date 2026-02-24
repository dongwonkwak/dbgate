#include "protocol/mysql_packet.hpp"

#include <format>

// ---------------------------------------------------------------------------
// MysqlPacket — 구현
//
// MySQL 와이어 포맷:
//   [3바이트 payload length LE][1바이트 seq_id][N바이트 payload]
// ---------------------------------------------------------------------------

namespace {

// payload 첫 바이트로 PacketType을 판별한다.
// ERR(0xFF)와 EOF(0xFE, payload < 9)는 명확히 구분한다.
// 0x00은 OK로 처리한다 (HandshakeResponse와 같은 값이나 OK 우선).
auto detect_packet_type(std::span<const std::uint8_t> payload) noexcept -> PacketType {
    if (payload.empty()) {
        return PacketType::kUnknown;
    }

    const std::uint8_t first = payload[0];

    switch (first) {
        case 0xFF:
            return PacketType::kError;
        case 0xFE:
            // EOF packet: payload가 9바이트 미만이어야 진짜 EOF
            if (payload.size() < 9) {
                return PacketType::kEof;
            }
            return PacketType::kUnknown;
        case 0x0A:
            return PacketType::kHandshake;
        case 0x03:
            return PacketType::kComQuery;
        case 0x01:
            return PacketType::kComQuit;
        case 0x00:
            // kOk 우선 (kHandshakeResponse와 동일 값이므로 context에 따라 결정)
            return PacketType::kOk;
        default:
            return PacketType::kUnknown;
    }
}

}  // namespace

// static
auto MysqlPacket::parse(std::span<const std::uint8_t> data)
    -> std::expected<MysqlPacket, ParseError>
{
    // 헤더 최소 4바이트 검증
    if (data.size() < 4) {
        return std::unexpected(ParseError{
            ParseErrorCode::kMalformedPacket,
            "packet too short",
            std::format("received {} bytes, need at least 4", data.size())
        });
    }

    // payload 길이: 3바이트 리틀 엔디언
    const std::uint32_t length =
        static_cast<std::uint32_t>(data[0])
        | (static_cast<std::uint32_t>(data[1]) << 8U)
        | (static_cast<std::uint32_t>(data[2]) << 16U);

    // 실제 데이터가 헤더(4) + payload 길이만큼 있어야 한다
    if (data.size() < static_cast<std::size_t>(4) + length) {
        return std::unexpected(ParseError{
            ParseErrorCode::kMalformedPacket,
            "incomplete payload",
            std::format("declared length={}, available={}", length, data.size() - 4)
        });
    }

    MysqlPacket pkt;
    pkt.sequence_id_ = data[3];

    // payload 복사
    const auto* payload_begin = data.data() + 4;
    pkt.payload_.assign(payload_begin, payload_begin + length);

    // PacketType 판별
    pkt.type_ = detect_packet_type(std::span<const std::uint8_t>{pkt.payload_});

    return pkt;
}

auto MysqlPacket::sequence_id() const noexcept -> std::uint8_t {
    return sequence_id_;
}

auto MysqlPacket::payload_length() const noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(payload_.size());
}

auto MysqlPacket::payload() const noexcept -> std::span<const std::uint8_t> {
    return std::span<const std::uint8_t>{payload_};
}

auto MysqlPacket::type() const noexcept -> PacketType {
    return type_;
}

auto MysqlPacket::serialize() const -> std::vector<std::uint8_t> {
    const std::uint32_t len = payload_length();

    // MySQL 와이어 프로토콜: payload 길이 필드는 3바이트 = 최대 0xFFFFFF(16MB-1)
    static constexpr std::uint32_t kMaxPayloadLen = 0x00FFFFFFU;
    if (len > kMaxPayloadLen) {
        // 상한 초과 시 빈 벡터 반환 (호출자가 에러로 처리해야 함)
        return {};
    }

    // 헤더 4바이트 + payload를 한 번에 구성
    std::vector<std::uint8_t> result(4 + len);

    // 3바이트 payload 길이 (리틀 엔디언)
    result[0] = static_cast<std::uint8_t>(len & 0xFFU);
    result[1] = static_cast<std::uint8_t>((len >> 8U) & 0xFFU);
    result[2] = static_cast<std::uint8_t>((len >> 16U) & 0xFFU);

    // sequence_id
    result[3] = sequence_id_;

    // payload
    if (len > 0) {
        std::copy(payload_.begin(), payload_.end(), result.begin() + 4);
    }

    return result;
}

// static
auto MysqlPacket::make_error(std::uint16_t    error_code,
                             std::string_view message,
                             std::uint8_t     sequence_id) -> MysqlPacket
{
    // MySQL ERR Packet payload 포맷:
    //   [0xFF][2바이트 error_code LE][#][5바이트 sql_state]["message"]
    static constexpr std::string_view kSqlState = "HY000";

    // MySQL 와이어 프로토콜: payload 길이 필드는 3바이트 = 최대 0xFFFFFF(16MB-1)
    // 고정 헤더(9바이트) + message가 0xFFFFFF를 초과하지 않도록 message를 잘라낸다.
    static constexpr std::size_t kMaxPayloadLen  = 0x00FFFFFFU;
    static constexpr std::size_t kFixedHeaderLen = 9U;  // 1(0xFF) + 2(code) + 1('#') + 5(state)

    // message를 최대 허용 길이로 truncation
    const std::size_t max_msg_len = kMaxPayloadLen - kFixedHeaderLen;
    const std::string_view safe_message =
        (message.size() > max_msg_len) ? message.substr(0, max_msg_len) : message;

    // payload 크기: 1 + 2 + 1 + 5 + safe_message.size()
    const std::size_t payload_size = kFixedHeaderLen + safe_message.size();

    MysqlPacket pkt;
    pkt.sequence_id_ = sequence_id;
    pkt.type_        = PacketType::kError;
    pkt.payload_.resize(payload_size);

    std::size_t pos = 0;

    // 0xFF 마커
    pkt.payload_[pos++] = 0xFF;

    // error_code 2바이트 LE
    pkt.payload_[pos++] = static_cast<std::uint8_t>(error_code & 0xFFU);
    pkt.payload_[pos++] = static_cast<std::uint8_t>((error_code >> 8U) & 0xFFU);

    // '#' + sql_state (5바이트)
    pkt.payload_[pos++] = static_cast<std::uint8_t>('#');
    for (char c : kSqlState) {
        pkt.payload_[pos++] = static_cast<std::uint8_t>(c);
    }

    // message (safe_message: truncated to fit within 0xFFFFFF payload limit)
    for (char c : safe_message) {
        pkt.payload_[pos++] = static_cast<std::uint8_t>(c);
    }

    return pkt;
}
