// fuzz_policy_engine.cpp — libFuzzer target for policy engine
//
// Targets: PolicyEngine::evaluate(), PolicyEngine::evaluate_error()
// Strategy: Fixed PolicyConfig + FuzzedDataProvider for SessionContext + SQL
// Dependencies: parser/sql_parser, parser/injection_detector,
//               parser/procedure_detector, policy/policy_engine, spdlog (LEVEL_OFF)

#include <fuzzer/FuzzedDataProvider.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "common/types.hpp"
#include "parser/sql_parser.hpp"
#include "policy/policy_engine.hpp"
#include "policy/rule.hpp"

namespace {

// Build a realistic PolicyConfig for testing
auto make_test_config() -> std::shared_ptr<PolicyConfig> {
    auto config = std::make_shared<PolicyConfig>();

    // Global settings
    config->global.log_level = "info";
    config->global.log_format = "json";
    config->global.max_connections = 100;
    config->global.connection_timeout_sec = 30;

    // SQL rules — block dangerous statements
    config->sql_rules.block_statements = {"DROP", "TRUNCATE"};
    config->sql_rules.block_patterns = {".*UNION.*SELECT.*", ".*OR\\s+1\\s*=\\s*1.*"};

    // Access control — allow app_user on specific tables
    AccessRule rule;
    rule.user = "app_user";
    rule.source_ip_cidr = "192.168.0.0/16";
    rule.allowed_tables = {"users", "orders", "products"};
    rule.allowed_operations = {"SELECT", "INSERT", "UPDATE"};
    rule.blocked_operations = {"DROP", "TRUNCATE"};
    config->access_control.push_back(rule);

    // Wildcard user rule
    AccessRule wildcard_rule;
    wildcard_rule.user = "*";
    wildcard_rule.allowed_tables = {"*"};
    wildcard_rule.allowed_operations = {"SELECT"};
    config->access_control.push_back(wildcard_rule);

    // Procedure control
    config->procedure_control.mode = "whitelist";
    config->procedure_control.whitelist = {"sp_get_user", "sp_list_orders"};
    config->procedure_control.block_dynamic_sql = true;
    config->procedure_control.block_create_alter = true;

    // Data protection
    config->data_protection.max_result_rows = 10000;
    config->data_protection.block_schema_access = true;

    return config;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Use a static config to avoid repeated allocation
    static auto config = make_test_config();
    static PolicyEngine engine(config);

    FuzzedDataProvider provider(data, size);

    // Construct SessionContext from fuzzed data
    SessionContext session;
    session.session_id = provider.ConsumeIntegral<uint64_t>();
    session.client_ip = provider.ConsumeRandomLengthString(45);  // max IPv6 length
    session.client_port = provider.ConsumeIntegral<uint16_t>();
    session.db_user = provider.ConsumeRandomLengthString(32);  // max MySQL user
    session.db_name = provider.ConsumeRandomLengthString(64);  // max MySQL db name
    session.handshake_done = provider.ConsumeBool();

    // Remaining bytes become the SQL query
    auto sql = provider.ConsumeRemainingBytesAsString();

    // Parse SQL
    SqlParser parser;
    auto parse_result = parser.parse(std::string_view{sql});

    if (parse_result.has_value()) {
        // Evaluate policy with parsed query
        [[maybe_unused]] auto result = engine.evaluate(parse_result.value(), session);
    } else {
        // Exercise the error path — fail-close invariant: must always return kBlock
        auto result = engine.evaluate_error(parse_result.error(), session);
        if (result.action != PolicyAction::kBlock) {
            __builtin_trap();  // fail-close 위반: 퍼저/CI가 즉시 실패해야 함
        }
    }

    return 0;
}
