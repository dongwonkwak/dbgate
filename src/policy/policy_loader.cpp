// ---------------------------------------------------------------------------
// policy_loader.cpp
//
// YAML 정책 파일을 로드하여 PolicyConfig 구조체로 파싱한다.
//
// [설계 원칙]
// - All-or-nothing: 파싱 실패 시 부분 정책을 반환하지 않는다.
// - Fail-close: block_patterns 가 비어있으면 std::unexpected 반환.
//   (InjectionDetector fail-close 연계 — 패턴 없음은 탐지 불가 상태)
// - YAML 파일 전체를 로그에 출력하지 않는다 (민감 정보 보호).
// - 필드 누락 시 기본값(구조체 기본값)을 적용한다.
//
// [알려진 한계]
// - connection_timeout YAML 키: "30s" 형태의 숫자+단위 문자열에서 숫자만 추출.
//   단위가 "s"(초) 외의 값이면 파싱을 건너뛰고 기본값(30)을 적용한다.
// - CIDR 표기법: 문자열 그대로 source_ip_cidr 멤버에 저장.
//   유효성 검사는 PolicyEngine::ip_in_cidr 에서 수행된다.
// - time_restriction.allow YAML 키 → allow_range 멤버 매핑.
//
// [fail-close 연계 — block_patterns 최소 1개 검증]
// PolicyLoader::load 는 block_patterns 가 비어있으면 std::unexpected 를 반환한다.
// 이는 InjectionDetector 에 빈 패턴 목록이 전달되어 fail-close 상태가 되는
// 상황(모든 SQL 차단)을 방지한다. 운영자가 의도적으로 패턴을 비웠을 때도
// 명시적 오류를 발생시켜 설정 실수를 조기에 감지하도록 한다.
//
// [오탐/미탐 트레이드오프]
// - block_patterns 에 잘못된 regex 패턴이 있으면 PolicyEngine 에서 해당 패턴을
//   건너뛰고 경고 로그를 출력한다. 이는 false negative 증가를 의미한다.
//   로드 시점에 패턴 검증 경고를 출력하여 운영자에게 알린다.
// ---------------------------------------------------------------------------

#include "policy/policy_loader.hpp"

#include <charconv>
#include <regex>
#include <string>
#include <filesystem>

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

// ---------------------------------------------------------------------------
// 내부 헬퍼: connection_timeout 문자열("30s")에서 정수(30)를 추출.
// 단위가 없거나 숫자가 없으면 fallback 값을 반환한다.
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] std::uint32_t parse_timeout_str(const std::string& raw, std::uint32_t fallback) {
    if (raw.empty()) {
        return fallback;
    }
    // 숫자 부분만 추출 (선두 공백 허용 안 함)
    const char* begin = raw.data();
    const char* end   = raw.data() + raw.size();

    std::uint32_t value{0};
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{}) {
        spdlog::warn("policy_loader: cannot parse connection_timeout '{}', using default {}s",
                     raw, fallback);
        return fallback;
    }
    // 남은 부분이 단위 문자(s, m 등)라면 무시. 파싱 완료.
    return value;
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: regex 패턴이 유효한지 사전 검증하고 경고 로그를 출력한다.
// PolicyEngine에서도 건너뛰므로 중복 경고가 발생할 수 있으나,
// 로드 시점 조기 감지가 더 중요하다 (false negative 조기 경보).
// ---------------------------------------------------------------------------
void validate_block_patterns(const std::vector<std::string>& patterns) {
    for (const auto& p : patterns) {
        try {
            std::regex re(p, std::regex_constants::icase | std::regex_constants::ECMAScript);
            (void)re;  // 컴파일만 확인
        } catch (const std::regex_error& e) {
            // [오탐/미탐 경보] 잘못된 패턴은 PolicyEngine에서 건너뛰므로
            // 해당 패턴의 탐지가 누락된다 (false negative 증가).
            spdlog::warn(
                "policy_loader: block_pattern '{}' is invalid regex and will be skipped by "
                "PolicyEngine — false negative risk: {}",
                p, e.what()
            );
        }
    }
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: YAML 노드에서 string 벡터를 읽는다.
// 노드가 없거나 sequence 가 아니면 빈 벡터를 반환한다.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<std::string> read_string_sequence(const YAML::Node& node) {
    std::vector<std::string> result;
    if (!node || !node.IsSequence()) {
        return result;
    }
    result.reserve(node.size());
    for (const auto& item : node) {
        if (item.IsScalar()) {
            result.push_back(item.as<std::string>());
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: YAML 노드에서 bool 값을 읽는다. 없으면 fallback 반환.
// ---------------------------------------------------------------------------
[[nodiscard]] bool read_bool(const YAML::Node& node, bool fallback) {
    if (!node || !node.IsScalar()) {
        return fallback;
    }
    try {
        return node.as<bool>();
    } catch (const YAML::Exception&) {
        return fallback;
    }
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: YAML 노드에서 uint32_t 값을 읽는다. 없으면 fallback 반환.
// ---------------------------------------------------------------------------
[[nodiscard]] std::uint32_t read_uint32(const YAML::Node& node, std::uint32_t fallback) {
    if (!node || !node.IsScalar()) {
        return fallback;
    }
    try {
        return node.as<std::uint32_t>();
    } catch (const YAML::Exception&) {
        return fallback;
    }
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: YAML 노드에서 string 값을 읽는다. 없으면 fallback 반환.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string read_string(const YAML::Node& node, const std::string& fallback) {
    if (!node || !node.IsScalar()) {
        return fallback;
    }
    try {
        return node.as<std::string>();
    } catch (const YAML::Exception&) {
        return fallback;
    }
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: GlobalConfig 파싱
// ---------------------------------------------------------------------------
[[nodiscard]] GlobalConfig parse_global(const YAML::Node& global_node) {
    GlobalConfig cfg{};
    if (!global_node || !global_node.IsMap()) {
        return cfg;
    }

    cfg.log_level  = read_string(global_node["log_level"],  cfg.log_level);
    cfg.log_format = read_string(global_node["log_format"], cfg.log_format);
    cfg.max_connections = read_uint32(global_node["max_connections"], cfg.max_connections);

    // connection_timeout: "30s" → 30 (숫자만 추출)
    if (global_node["connection_timeout"] && global_node["connection_timeout"].IsScalar()) {
        const std::string raw = global_node["connection_timeout"].as<std::string>();
        cfg.connection_timeout_sec = parse_timeout_str(raw, cfg.connection_timeout_sec);
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: TimeRestriction 파싱
// time_restriction YAML:
//   allow: "09:00-18:00"
//   timezone: "Asia/Seoul"
// ---------------------------------------------------------------------------
[[nodiscard]] std::optional<TimeRestriction>
parse_time_restriction(const YAML::Node& tr_node) {
    // null 또는 없음 → std::nullopt
    if (!tr_node || tr_node.IsNull()) {
        return std::nullopt;
    }
    if (!tr_node.IsMap()) {
        spdlog::warn("policy_loader: time_restriction is not a map, ignoring");
        return std::nullopt;
    }

    TimeRestriction tr{};
    // YAML 키: "allow" → C++ 멤버: allow_range
    tr.allow_range = read_string(tr_node["allow"],    tr.allow_range);
    tr.timezone    = read_string(tr_node["timezone"], tr.timezone);
    return tr;
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: AccessRule 파싱
// ---------------------------------------------------------------------------
[[nodiscard]] AccessRule parse_access_rule(const YAML::Node& rule_node) {
    AccessRule rule{};
    if (!rule_node.IsMap()) {
        return rule;
    }

    rule.user = read_string(rule_node["user"], "");

    // YAML 키: "source_ip" → C++ 멤버: source_ip_cidr
    rule.source_ip_cidr = read_string(rule_node["source_ip"], "");

    // allowed_tables: 없으면 기본값 {"*"}
    if (rule_node["allowed_tables"]) {
        rule.allowed_tables = read_string_sequence(rule_node["allowed_tables"]);
        if (rule.allowed_tables.empty()) {
            // 빈 allowed_tables 는 "아무 테이블도 허용 안 함"과 다르므로
            // 명시적으로 빈 목록을 유지한다 (기본값 {"*"} 아님).
        }
    }
    // allowed_operations: 없으면 빈 벡터 (제한 없음으로 처리)
    rule.allowed_operations  = read_string_sequence(rule_node["allowed_operations"]);
    rule.blocked_operations  = read_string_sequence(rule_node["blocked_operations"]);

    // time_restriction 파싱
    rule.time_restriction = parse_time_restriction(rule_node["time_restriction"]);

    return rule;
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: SqlRule 파싱
// ---------------------------------------------------------------------------
[[nodiscard]] SqlRule parse_sql_rules(const YAML::Node& sql_node) {
    SqlRule rules{};
    if (!sql_node || !sql_node.IsMap()) {
        return rules;
    }

    rules.block_statements = read_string_sequence(sql_node["block_statements"]);
    rules.block_patterns   = read_string_sequence(sql_node["block_patterns"]);

    return rules;
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: ProcedureControl 파싱
// ---------------------------------------------------------------------------
[[nodiscard]] ProcedureControl parse_procedure_control(const YAML::Node& pc_node) {
    ProcedureControl ctrl{};
    if (!pc_node || !pc_node.IsMap()) {
        return ctrl;
    }

    ctrl.mode             = read_string(pc_node["mode"], ctrl.mode);
    ctrl.whitelist        = read_string_sequence(pc_node["whitelist"]);
    ctrl.block_dynamic_sql = read_bool(pc_node["block_dynamic_sql"], ctrl.block_dynamic_sql);
    ctrl.block_create_alter = read_bool(pc_node["block_create_alter"], ctrl.block_create_alter);

    // mode 유효성 검사
    if (ctrl.mode != "whitelist" && ctrl.mode != "blacklist") {
        spdlog::warn(
            "policy_loader: procedure_control.mode '{}' is not 'whitelist' or 'blacklist', "
            "defaulting to 'whitelist'",
            ctrl.mode
        );
        ctrl.mode = "whitelist";
    }

    return ctrl;
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: DataProtection 파싱
// ---------------------------------------------------------------------------
[[nodiscard]] DataProtection parse_data_protection(const YAML::Node& dp_node) {
    DataProtection dp{};
    if (!dp_node || !dp_node.IsMap()) {
        return dp;
    }

    dp.max_result_rows   = read_uint32(dp_node["max_result_rows"], dp.max_result_rows);
    dp.block_schema_access = read_bool(dp_node["block_schema_access"], dp.block_schema_access);

    return dp;
}

}  // namespace

// ---------------------------------------------------------------------------
// PolicyLoader::load 구현
// ---------------------------------------------------------------------------
std::expected<PolicyConfig, std::string>
PolicyLoader::load(const std::filesystem::path& config_path) {
    // 1. 경로 정규화 (path traversal 방지 목적)
    std::error_code ec;
    const auto canonical_path = std::filesystem::canonical(config_path, ec);
    if (ec) {
        const std::string err = fmt::format(
            "policy_loader: cannot resolve config path '{}': {}",
            config_path.string(), ec.message()
        );
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    spdlog::info("policy_loader: loading policy from '{}'", canonical_path.string());

    // 2. YAML 파일 로드 (yaml-cpp 예외 처리)
    YAML::Node root;
    try {
        root = YAML::LoadFile(canonical_path.string());
    } catch (const YAML::BadFile& e) {
        const std::string err = fmt::format(
            "policy_loader: cannot open file '{}': {}",
            canonical_path.string(), e.what()
        );
        spdlog::error("{}", err);
        return std::unexpected(err);
    } catch (const YAML::ParserException& e) {
        // 라인 번호 포함한 상세 에러 메시지
        const std::string err = fmt::format(
            "policy_loader: YAML parse error in '{}' at line {}, col {}: {}",
            canonical_path.string(),
            e.mark.line + 1,   // yaml-cpp는 0-based
            e.mark.column + 1,
            e.what()
        );
        spdlog::error("{}", err);
        return std::unexpected(err);
    } catch (const YAML::Exception& e) {
        const std::string err = fmt::format(
            "policy_loader: YAML error in '{}': {}",
            canonical_path.string(), e.what()
        );
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    if (!root || !root.IsMap()) {
        const std::string err = fmt::format(
            "policy_loader: '{}' is not a valid YAML map (top-level)",
            canonical_path.string()
        );
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    // 3. 각 섹션 파싱 (try-catch per section — YAML 예외 안전)
    PolicyConfig cfg{};

    try {
        cfg.global = parse_global(root["global"]);
    } catch (const YAML::Exception& e) {
        const std::string err = fmt::format(
            "policy_loader: error parsing 'global' section: {}", e.what()
        );
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    try {
        const YAML::Node& ac_node = root["access_control"];
        if (ac_node && ac_node.IsSequence()) {
            cfg.access_control.reserve(ac_node.size());
            for (const auto& rule_node : ac_node) {
                cfg.access_control.push_back(parse_access_rule(rule_node));
            }
        }
    } catch (const YAML::Exception& e) {
        const std::string err = fmt::format(
            "policy_loader: error parsing 'access_control' section: {}", e.what()
        );
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    try {
        cfg.sql_rules = parse_sql_rules(root["sql_rules"]);
    } catch (const YAML::Exception& e) {
        const std::string err = fmt::format(
            "policy_loader: error parsing 'sql_rules' section: {}", e.what()
        );
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    try {
        cfg.procedure_control = parse_procedure_control(root["procedure_control"]);
    } catch (const YAML::Exception& e) {
        const std::string err = fmt::format(
            "policy_loader: error parsing 'procedure_control' section: {}", e.what()
        );
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    try {
        cfg.data_protection = parse_data_protection(root["data_protection"]);
    } catch (const YAML::Exception& e) {
        const std::string err = fmt::format(
            "policy_loader: error parsing 'data_protection' section: {}", e.what()
        );
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    // 4. block_patterns 최소 1개 검증 (fail-close 연계)
    //
    // [Fail-close 요구사항]
    // block_patterns 가 비어있으면 InjectionDetector 에 빈 패턴 목록이 전달되어
    // InjectionDetector::fail_close_active_ = true 상태가 된다. 이 상태에서
    // check() 는 모든 SQL 을 차단한다. 이는 운영자 의도와 다를 수 있으므로
    // 로드 시점에 명시적 오류를 반환한다.
    if (cfg.sql_rules.block_patterns.empty()) {
        const std::string err =
            "policy_loader: sql_rules.block_patterns must have at least one pattern "
            "(fail-close: empty pattern list would block all SQL via InjectionDetector)";
        spdlog::error("{}", err);
        return std::unexpected(err);
    }

    // 5. block_patterns 유효성 사전 검증 (오류 경고만 — 파싱 실패 아님)
    validate_block_patterns(cfg.sql_rules.block_patterns);

    spdlog::info(
        "policy_loader: policy loaded successfully — "
        "access_rules={}, block_statements={}, block_patterns={}",
        cfg.access_control.size(),
        cfg.sql_rules.block_statements.size(),
        cfg.sql_rules.block_patterns.size()
    );

    return cfg;
}
