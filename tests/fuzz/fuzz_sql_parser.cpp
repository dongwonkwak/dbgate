// fuzz_sql_parser.cpp — libFuzzer target for SQL parser
//
// Targets: SqlParser::parse()
// Dependencies: parser/sql_parser, parser/injection_detector,
//               parser/procedure_detector, spdlog (LEVEL_OFF)

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "parser/sql_parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Interpret raw bytes as a SQL string
    auto sql = std::string_view{reinterpret_cast<const char*>(data), size};

    SqlParser parser;
    auto result = parser.parse(sql);

    if (result.has_value()) {
        // Exercise accessors on successful parse
        [[maybe_unused]] auto cmd = result.value().command;
        [[maybe_unused]] auto tables = result.value().tables;
        [[maybe_unused]] auto raw = result.value().raw_sql;
        [[maybe_unused]] auto where = result.value().has_where_clause;
        [[maybe_unused]] auto multi = result.value().has_multi_statement;
    }

    return 0;
}
