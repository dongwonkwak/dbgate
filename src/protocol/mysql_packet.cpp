#include "protocol/mysql_packet.hpp"

// ---------------------------------------------------------------------------
// MysqlPacket — stub implementation
//   모든 메서드는 최소한의 기본값을 반환한다.
//   실제 구현은 별도 DON 태스크에서 진행된다.
// ---------------------------------------------------------------------------

// static
auto MysqlPacket::parse(std::span<const std::uint8_t> /*data*/)
    -> std::expected<MysqlPacket, ParseError>
{
    return std::unexpected(ParseError{
        ParseErrorCode::kInternalError,
        "MysqlPacket::parse: not implemented",
        {}
    });
}

auto MysqlPacket::sequence_id() const noexcept -> std::uint8_t
{
    return sequence_id_;
}

auto MysqlPacket::payload_length() const noexcept -> std::uint32_t
{
    return static_cast<std::uint32_t>(payload_.size());
}

auto MysqlPacket::payload() const noexcept -> std::span<const std::uint8_t>
{
    return std::span<const std::uint8_t>{payload_};
}

auto MysqlPacket::type() const noexcept -> PacketType
{
    return type_;
}

auto MysqlPacket::serialize() const -> std::vector<std::uint8_t>
{
    return {};
}

// static
auto MysqlPacket::make_error(std::uint16_t   /*error_code*/,
                             std::string_view /*message*/,
                             std::uint8_t     /*sequence_id*/) -> MysqlPacket
{
    return MysqlPacket{};
}
