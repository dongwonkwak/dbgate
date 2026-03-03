// fuzz_mysql_packet.cpp — libFuzzer target for MySQL packet parsing
//
// Targets: MysqlPacket::parse(), extract_command()
// Dependencies: protocol/mysql_packet, protocol/command (no spdlog)

#include <cstddef>
#include <cstdint>
#include <span>

#include "protocol/command.hpp"
#include "protocol/mysql_packet.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Phase 1: Fuzz MysqlPacket::parse with raw bytes
    auto packet_result = MysqlPacket::parse(std::span<const uint8_t>{data, size});

    if (packet_result.has_value()) {
        // Phase 2: If parse succeeds, fuzz extract_command
        auto cmd_result = extract_command(packet_result.value());

        // Phase 3: Exercise serialize round-trip
        [[maybe_unused]] auto serialized = packet_result.value().serialize();

        // Phase 4: Exercise accessors
        [[maybe_unused]] auto seq = packet_result.value().sequence_id();
        [[maybe_unused]] auto len = packet_result.value().payload_length();
        [[maybe_unused]] auto typ = packet_result.value().type();
        [[maybe_unused]] auto pay = packet_result.value().payload();
    }

    return 0;  // Non-zero return values are reserved for future use
}
