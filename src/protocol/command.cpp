#include "protocol/command.hpp"

#include <format>

// ---------------------------------------------------------------------------
// extract_command — 구현
//
// MysqlPacket의 payload 첫 바이트를 CommandType으로 매핑한다.
// COM_QUERY(0x03)의 경우 나머지 바이트를 SQL 문자열로 설정한다.
// ---------------------------------------------------------------------------

namespace {

// 커맨드 바이트를 CommandType으로 매핑한다.
// 알려진 값이 아니면 std::nullopt를 반환한다.
auto map_command_byte(std::uint8_t byte) noexcept
    -> std::optional<CommandType>
{
    switch (byte) {
        case 0x01: return CommandType::kComQuit;
        case 0x02: return CommandType::kComInitDb;
        case 0x03: return CommandType::kComQuery;
        case 0x04: return CommandType::kComFieldList;
        case 0x05: return CommandType::kComCreateDb;
        case 0x06: return CommandType::kComDropDb;
        case 0x07: return CommandType::kComRefresh;
        case 0x09: return CommandType::kComStatistics;
        case 0x0A: return CommandType::kComProcessInfo;
        case 0x0B: return CommandType::kComConnect;
        case 0x0C: return CommandType::kComProcessKill;
        case 0x0E: return CommandType::kComPing;
        case 0x16: return CommandType::kComStmtPrepare;
        case 0x17: return CommandType::kComStmtExecute;
        case 0x19: return CommandType::kComStmtClose;
        case 0x1A: return CommandType::kComStmtReset;
        default:   return std::nullopt;
    }
}

}  // namespace

auto extract_command(const MysqlPacket& packet)
    -> std::expected<CommandPacket, ParseError>
{
    const auto payload = packet.payload();

    // payload가 비어있으면 malformed
    if (payload.empty()) {
        return std::unexpected(ParseError{
            ParseErrorCode::kMalformedPacket,
            "empty payload",
            {}
        });
    }

    const std::uint8_t cmd_byte = payload[0];
    const auto cmd_type = map_command_byte(cmd_byte);

    if (!cmd_type.has_value()) {
        return std::unexpected(ParseError{
            ParseErrorCode::kUnsupportedCommand,
            std::format("unknown command byte: 0x{:02X}", cmd_byte),
            {}
        });
    }

    CommandPacket cmd;
    cmd.command_type = *cmd_type;
    cmd.sequence_id  = packet.sequence_id();

    // COM_QUERY: payload[1:]을 SQL 문자열로 설정
    if (*cmd_type == CommandType::kComQuery && payload.size() > 1) {
        cmd.query.assign(
            reinterpret_cast<const char*>(payload.data() + 1),
            payload.size() - 1
        );
    }

    return cmd;
}
