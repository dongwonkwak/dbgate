#include "protocol/command.hpp"

// ---------------------------------------------------------------------------
// extract_command — stub implementation
// ---------------------------------------------------------------------------

auto extract_command(const MysqlPacket& /*packet*/)
    -> std::expected<CommandPacket, ParseError>
{
    return std::unexpected(ParseError{
        ParseErrorCode::kInternalError,
        "extract_command: not implemented",
        {}
    });
}
