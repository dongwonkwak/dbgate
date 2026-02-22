#pragma once

#include "common/types.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// PacketType
//   MySQL 패킷의 종류를 식별한다.
//   핸드셰이크 완료 전에는 kHandshake / kHandshakeResponse,
//   그 이후에는 커맨드 계열(kComQuery 등)이 주로 사용된다.
// ---------------------------------------------------------------------------
enum class PacketType : std::uint8_t {
    kHandshake         = 0x0A,  // Server Handshake (Initial Handshake Packet)
    kHandshakeResponse = 0x00,  // Client Handshake Response (login request)
    kComQuery          = 0x03,  // COM_QUERY
    kComQuit           = 0x01,  // COM_QUIT
    kOk                = 0x00,  // OK Packet (context-dependent)
    kError             = 0xFF,  // ERR Packet
    kEof               = 0xFE,  // EOF Packet
    kUnknown           = 0x00,  // 미분류 / 파싱 불가
};

// ---------------------------------------------------------------------------
// MysqlPacket
//   MySQL 와이어 프로토콜의 단일 패킷을 나타낸다.
//
//   Wire 포맷:
//     [3바이트 payload length][1바이트 sequence id][payload...]
//
//   parse()  : 원시 바이트에서 MysqlPacket 으로 변환
//   serialize(): MysqlPacket 을 원시 바이트로 변환
//   make_error(): ERR Packet 을 생성하는 정적 팩토리
// ---------------------------------------------------------------------------
class MysqlPacket {
public:
    // 기본 생성자 (빈 패킷)
    MysqlPacket() = default;

    // -----------------------------------------------------------------------
    // parse
    //   data 는 MySQL 와이어 포맷 원시 바이트 전체를 담고 있어야 한다.
    //   헤더(4바이트)와 페이로드가 모두 포함되어 있어야 파싱이 성공한다.
    //
    //   실패 시: std::unexpected(ParseError)
    // -----------------------------------------------------------------------
    static auto parse(std::span<const std::uint8_t> data)
        -> std::expected<MysqlPacket, ParseError>;

    // -----------------------------------------------------------------------
    // Accessors (const noexcept)
    // -----------------------------------------------------------------------
    [[nodiscard]] auto sequence_id()     const noexcept -> std::uint8_t;
    [[nodiscard]] auto payload_length()  const noexcept -> std::uint32_t;
    [[nodiscard]] auto payload()         const noexcept
        -> std::span<const std::uint8_t>;
    [[nodiscard]] auto type()            const noexcept -> PacketType;

    // -----------------------------------------------------------------------
    // serialize
    //   헤더(4바이트) + 페이로드를 이어붙인 바이트 벡터를 반환한다.
    // -----------------------------------------------------------------------
    [[nodiscard]] auto serialize() const -> std::vector<std::uint8_t>;

    // -----------------------------------------------------------------------
    // make_error
    //   MySQL ERR Packet 을 생성한다.
    //   error_code  : MySQL error code (2바이트)
    //   message     : Human-readable error message
    //   sequence_id : 응답 시퀀스 ID
    // -----------------------------------------------------------------------
    static auto make_error(std::uint16_t   error_code,
                           std::string_view message,
                           std::uint8_t    sequence_id) -> MysqlPacket;

private:
    std::uint8_t              sequence_id_{0};
    std::vector<std::uint8_t> payload_{};
    PacketType                type_{PacketType::kUnknown};
};
