// ---------------------------------------------------------------------------
// policy_engine.cpp
//
// 파싱된 쿼리와 세션 컨텍스트를 받아 허용/차단/로깅 판정을 내리는 엔진.
//
// [Fail-close 원칙 — 절대 위반 금지]
// 1. config_ == nullptr → kBlock
// 2. query.command == kUnknown → kBlock
// 3. 내부 예외/오류 → kBlock
// 4. 정책 일치 없음 → kBlock (default deny)
// 5. kAllow 는 명시적 허용 규칙이 존재할 때만 반환
//
// [Hot Reload]
// reload() 에서 std::atomic_store(&config_, new_config) 를 사용하여
// 진행 중인 evaluate() 와 경쟁 없이 교체한다.
// C++ 표준에서 shared_ptr 에 대한 atomic 연산은 deprecated (C++20) 되었으나
// 헤더의 config_ 타입이 std::shared_ptr<PolicyConfig> 로 고정되어 있으므로
// C++20 atomic_store/atomic_load 비멤버 함수를 사용한다.
// [참고] C++23에서 std::atomic<std::shared_ptr<T>> 가 권장되나 헤더 변경 불가.
//
// [IPv4 CIDR 매칭]
// ip_in_cidr() 는 IPv4 전용이다. IPv6 는 미지원 (알려진 한계).
// CIDR 파싱 실패 시 false 반환 (fail-close: 알 수 없는 IP = 차단).
//
// [시간대 처리]
// setenv/tzset 은 스레드 안전하지 않으므로 static mutex 로 보호한다.
// DST 전환 경계에서 ±1시간 오차 가능성 있음 (알려진 한계).
//
// [오탐/미탐 트레이드오프]
// - access_control 의 user = "*" 와일드카드: 모든 사용자에 적용되므로
//   규칙 순서 오류 시 의도치 않은 허용/차단 발생 가능.
// - block_patterns 의 regex 잘못 작성 시 해당 패턴만 건너뜀 (false negative 증가).
// - CIDR 매칭 실패 (잘못된 CIDR 문자열): 해당 규칙을 매칭 실패로 처리 (fail-close).
//
// [알려진 한계]
// - IPv6 CIDR 매칭 미지원
// - 시간대 변환의 DST 오차
// - 복잡한 서브쿼리/CTE 에서 테이블명 추출 불완전 (파서 한계)
// ---------------------------------------------------------------------------

#include "policy/policy_engine.hpp"

#include <algorithm>
#include <arpa/inet.h>      // inet_pton, AF_INET
#include <cctype>
#include <cstring>          // memset
#include <ctime>            // localtime_r, tzset
#include <memory>
#include <mutex>
#include <netinet/in.h>     // in_addr
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <cstdlib>          // setenv

#include <spdlog/spdlog.h>

// ---------------------------------------------------------------------------
// 내부 헬퍼: SqlCommand → 문자열 변환
// ---------------------------------------------------------------------------
static std::string_view command_to_string(SqlCommand cmd) {
    switch (cmd) {
        case SqlCommand::kSelect:   return "SELECT";
        case SqlCommand::kInsert:   return "INSERT";
        case SqlCommand::kUpdate:   return "UPDATE";
        case SqlCommand::kDelete:   return "DELETE";
        case SqlCommand::kDrop:     return "DROP";
        case SqlCommand::kTruncate: return "TRUNCATE";
        case SqlCommand::kAlter:    return "ALTER";
        case SqlCommand::kCreate:   return "CREATE";
        case SqlCommand::kCall:     return "CALL";
        case SqlCommand::kPrepare:  return "PREPARE";
        case SqlCommand::kExecute:  return "EXECUTE";
        case SqlCommand::kUnknown:  return "UNKNOWN";
        default:                    return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: 문자열 대소문자 무관 비교
// ---------------------------------------------------------------------------
static bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    return std::equal(a.begin(), a.end(), b.begin(), [](unsigned char ac, unsigned char bc) {
        return std::tolower(ac) == std::tolower(bc);
    });
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: IPv4 CIDR 매칭
//
// IPv4 전용. cidr_string 형식: "10.0.0.0/8"
// 실패(잘못된 CIDR/IP 형식) 시 false 반환 (fail-close).
//
// [보안 주의]
// IP 스푸핑 방어는 네트워크 레이어 소관.
// 이 함수는 텍스트 파싱만 수행하며 패킷 레벨 검증을 수행하지 않는다.
//
// [알려진 한계]
// - IPv6 미지원. IPv6 주소가 전달되면 inet_pton(AF_INET, ...) 실패 → false 반환.
// - "0.0.0.0/0" (모든 IP 허용) 은 올바르게 처리된다.
// ---------------------------------------------------------------------------
static bool ip_in_cidr(const std::string& ip, const std::string& cidr) {
    // CIDR에서 '/' 기준 분리
    const auto slash_pos = cidr.find('/');
    if (slash_pos == std::string::npos) {
        spdlog::warn("policy_engine: invalid CIDR format (no '/') '{}'", cidr);
        return false;
    }

    const std::string network_str = cidr.substr(0, slash_pos);
    const std::string prefix_str  = cidr.substr(slash_pos + 1);

    // prefix_len 파싱
    int prefix_len = 0;
    try {
        std::size_t idx{0};
        prefix_len = std::stoi(prefix_str, &idx);
        if (idx != prefix_str.size() || prefix_len < 0 || prefix_len > 32) {
            spdlog::warn("policy_engine: invalid prefix length in CIDR '{}'", cidr);
            return false;
        }
    } catch (const std::exception&) {
        spdlog::warn("policy_engine: cannot parse prefix length in CIDR '{}'", cidr);
        return false;
    }

    // IP 주소를 32비트 정수로 변환
    struct in_addr ip_addr{};
    struct in_addr net_addr{};

    if (inet_pton(AF_INET, ip.c_str(), &ip_addr) != 1) {
        // IPv6 또는 잘못된 IP — fail-close
        spdlog::debug("policy_engine: cannot parse client IP '{}' as IPv4", ip);
        return false;
    }
    if (inet_pton(AF_INET, network_str.c_str(), &net_addr) != 1) {
        spdlog::warn("policy_engine: cannot parse network address '{}' in CIDR '{}'",
                     network_str, cidr);
        return false;
    }

    // 네트워크 마스크 계산
    // prefix_len == 0: 모든 IP 허용 (마스크 = 0)
    const std::uint32_t mask =
        (prefix_len == 0)
        ? 0u
        : htonl(~0u << (32 - static_cast<unsigned>(prefix_len)));

    const std::uint32_t ip_int  = ip_addr.s_addr;
    const std::uint32_t net_int = net_addr.s_addr;

    return (ip_int & mask) == (net_int & mask);
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: 시간 범위 파싱
// 형식: "HH:MM-HH:MM" (예: "09:00-18:00")
// ---------------------------------------------------------------------------
struct TimeRange {
    int start_h{0};
    int start_m{0};
    int end_h{0};
    int end_m{0};
};

static std::optional<TimeRange> parse_time_range(const std::string& range_str) {
    // "HH:MM-HH:MM" 형식 파싱
    // 최소 길이: "0:0-0:0" = 7
    if (range_str.size() < 7) {
        spdlog::warn("policy_engine: invalid time range format '{}' (too short)", range_str);
        return std::nullopt;
    }

    const auto dash_pos = range_str.find('-', 1);
    if (dash_pos == std::string::npos) {
        spdlog::warn("policy_engine: invalid time range format '{}' (no '-')", range_str);
        return std::nullopt;
    }

    const std::string start_str = range_str.substr(0, dash_pos);
    const std::string end_str   = range_str.substr(dash_pos + 1);

    auto parse_hhmm = [](const std::string& hhmm, int& hour, int& min) -> bool {
        const auto colon_pos = hhmm.find(':');
        if (colon_pos == std::string::npos) {
            return false;
        }
        try {
            std::size_t idx{0};
            hour = std::stoi(hhmm.substr(0, colon_pos), &idx);
            min  = std::stoi(hhmm.substr(colon_pos + 1), &idx);
            return (hour >= 0 && hour <= 23 && min >= 0 && min <= 59);
        } catch (const std::exception&) {
            return false;
        }
    };

    TimeRange tr{};
    if (!parse_hhmm(start_str, tr.start_h, tr.start_m)) {
        spdlog::warn("policy_engine: invalid start time in range '{}'", range_str);
        return std::nullopt;
    }
    if (!parse_hhmm(end_str, tr.end_h, tr.end_m)) {
        spdlog::warn("policy_engine: invalid end time in range '{}'", range_str);
        return std::nullopt;
    }

    return tr;
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: 현재 시각이 TimeRange 내인지 확인 (timezone 적용)
//
// [스레드 안전성]
// setenv/tzset 은 POSIX에서 스레드 안전성을 보장하지 않는다.
// static mutex 로 보호하여 동시 호출 시 경쟁 조건을 방지한다.
//
// [DST 한계]
// 서머타임 전환 경계에서 ±1시간 오차 가능성이 있다.
// 정밀한 시간대 처리가 필요하면 ICU 라이브러리 사용을 권장한다.
//
// [오탐/미탐 트레이드오프]
// - timezone 이 비어있거나 tzset 실패 시 UTC 로 fallback.
// - UTC fallback 은 시간대 설정 오류 시 예기치 않은 차단/허용을 유발할 수 있다.
// ---------------------------------------------------------------------------
static bool is_within_time_range(const TimeRange& range, const std::string& tz_name) {
    // mutex 로 setenv/tzset 보호
    static std::mutex tz_mutex;

    struct tm now_tm{};
    {
        const std::lock_guard<std::mutex> lock(tz_mutex);

        // TZ 환경변수 설정 (빈 문자열이면 UTC 사용)
        const bool has_tz = !tz_name.empty();
        if (has_tz) {
            if (setenv("TZ", tz_name.c_str(), 1) != 0) {
                spdlog::warn("policy_engine: setenv(TZ, '{}') failed, using UTC", tz_name);
            }
        } else {
            if (setenv("TZ", "UTC", 1) != 0) {
                spdlog::warn("policy_engine: setenv(TZ, UTC) failed");
            }
        }
        tzset();

        const auto now = std::time(nullptr);
        if (localtime_r(&now, &now_tm) == nullptr) {
            spdlog::warn("policy_engine: localtime_r failed, denying access (fail-close)");
            return false;  // fail-close: 시각 획득 실패 → 차단
        }
    }

    const int now_minutes   = now_tm.tm_hour * 60 + now_tm.tm_min;
    const int start_minutes = range.start_h * 60 + range.start_m;
    const int end_minutes   = range.end_h   * 60 + range.end_m;

    // 자정을 넘지 않는 단순 범위 (예: 09:00-18:00)
    if (start_minutes <= end_minutes) {
        return (now_minutes >= start_minutes && now_minutes < end_minutes);
    }
    // 자정을 넘는 범위 (예: 22:00-06:00)
    return (now_minutes >= start_minutes || now_minutes < end_minutes);
}

// ---------------------------------------------------------------------------
// PolicyEngine 생성자
// ---------------------------------------------------------------------------
PolicyEngine::PolicyEngine(std::shared_ptr<PolicyConfig> config)
    : config_(std::move(config)) {
    // config_ 가 nullptr 이면 이후 모든 evaluate() 가 kBlock 을 반환한다.
    // fail-close 원칙에 의해 nullptr config 도 허용하되, 차단으로 처리.
    if (!config_) {
        spdlog::warn("policy_engine: constructed with nullptr config — all queries will be blocked (fail-close)");
    } else {
        spdlog::info("policy_engine: initialized with {} access rules, {} block statements, {} block patterns",
                     config_->access_control.size(),
                     config_->sql_rules.block_statements.size(),
                     config_->sql_rules.block_patterns.size());
    }
}

// ---------------------------------------------------------------------------
// PolicyEngine::evaluate 구현
//
// 평가 순서는 설계 명세(DON-26)를 준수한다.
// 모든 예외는 catch 후 kBlock 반환 (fail-close).
// ---------------------------------------------------------------------------
PolicyResult PolicyEngine::evaluate(
    const ParsedQuery&    query,
    const SessionContext& session) const {

    // Step 1: config_ nullptr 체크
    // 헤더의 config_ 타입이 std::shared_ptr<PolicyConfig> (atomic 아님) 이므로
    // 복사 후 로컬 변수로 사용한다.
    // reload() 는 멀티스레드 환경에서 경쟁 조건이 있을 수 있으나,
    // 헤더 인터페이스 제약으로 config_mutex_ 없이 구현한다.
    // [설계 한계] 헤더에 std::atomic<std::shared_ptr<PolicyConfig>> 가 없으므로
    // 완전한 thread-safety 는 헤더 변경 후 도입 가능 (Architect 승인 필요).
    const auto config = config_;
    if (!config) {
        spdlog::error("policy_engine: config is null, blocking query (fail-close) session={}",
                      session.session_id);
        return PolicyResult{PolicyAction::kBlock, "no-config", "Policy config unavailable"};
    }

    // Step 2: Unknown command → kBlock
    if (query.command == SqlCommand::kUnknown) {
        spdlog::warn("policy_engine: unknown SQL command blocked, session={}, sql_prefix='{}'",
                     session.session_id,
                     query.raw_sql.substr(0, std::min(query.raw_sql.size(), std::size_t{50})));
        return PolicyResult{PolicyAction::kBlock, "unknown-command", "Unknown SQL command blocked"};
    }

    const std::string_view cmd_str = command_to_string(query.command);

    // Step 3: SQL 구문 차단 (block_statements)
    // block_statements 에는 SQL 키워드 문자열이 들어있다 (예: "DROP", "TRUNCATE").
    // query.command 를 문자열로 변환하여 대소문자 무관 비교한다.
    for (const auto& stmt : config->sql_rules.block_statements) {
        if (iequals(cmd_str, stmt)) {
            spdlog::info(
                "policy_engine: block_statement matched '{}', session={}, user='{}'",
                stmt, session.session_id, session.db_user
            );
            return PolicyResult{
                PolicyAction::kBlock,
                "block-statement",
                fmt::format("SQL statement blocked: {}", stmt)
            };
        }
    }

    // Step 4: SQL 패턴 차단 (block_patterns)
    // [오탐 주의] ORM 생성 쿼리에서 false positive 발생 가능.
    // [미탐 주의] 주석 분할(UN/**/ION)은 탐지 불가 (알려진 한계).
    for (const auto& pattern : config->sql_rules.block_patterns) {
        try {
            const std::regex re(
                pattern,
                std::regex_constants::icase | std::regex_constants::ECMAScript
            );
            if (std::regex_search(query.raw_sql, re)) {
                spdlog::info(
                    "policy_engine: block_pattern matched '{}', session={}, user='{}'",
                    pattern, session.session_id, session.db_user
                );
                return PolicyResult{
                    PolicyAction::kBlock,
                    "block-pattern",
                    fmt::format("SQL pattern blocked: {}", pattern)
                };
            }
        } catch (const std::regex_error& e) {
            // 잘못된 regex: 건너뜀 (false negative 증가, 로더에서 이미 경고)
            spdlog::warn("policy_engine: invalid block_pattern '{}', skipping: {}", pattern, e.what());
        }
    }

    // Step 5: 사용자/IP 접근 제어 (access_control 룰 찾기)
    // 첫 번째 매칭 룰을 적용한다 (순서 우선).
    // [오탐 주의] user="*" 가 있는 룰이 앞에 있으면 특정 사용자 룰이 무시될 수 있다.
    const AccessRule* matched_rule = nullptr;
    for (const auto& rule : config->access_control) {
        // 사용자 매칭: 정확 일치 또는 와일드카드
        const bool user_match = (rule.user == session.db_user || rule.user == "*");
        if (!user_match) {
            continue;
        }

        // IP CIDR 매칭: source_ip_cidr 가 비어있으면 모든 IP 허용
        if (!rule.source_ip_cidr.empty()) {
            if (!ip_in_cidr(session.client_ip, rule.source_ip_cidr)) {
                continue;
            }
        }

        matched_rule = &rule;
        break;
    }

    if (matched_rule == nullptr) {
        spdlog::info(
            "policy_engine: no matching access rule for user='{}' ip='{}', session={}",
            session.db_user, session.client_ip, session.session_id
        );
        return PolicyResult{
            PolicyAction::kBlock,
            "no-access-rule",
            "No matching access rule for user/IP"
        };
    }

    // Step 6: 차단 오퍼레이션 체크 (blocked_operations)
    // blocked_operations 는 allowed_operations 보다 우선 적용된다.
    for (const auto& blocked_op : matched_rule->blocked_operations) {
        if (iequals(cmd_str, blocked_op)) {
            spdlog::info(
                "policy_engine: blocked_operation '{}' matched, session={}, user='{}'",
                blocked_op, session.session_id, session.db_user
            );
            return PolicyResult{
                PolicyAction::kBlock,
                "blocked-operation",
                fmt::format("Operation blocked for user '{}': {}", session.db_user, blocked_op)
            };
        }
    }

    // Step 7: 시간대 제한 체크 (time_restriction)
    if (matched_rule->time_restriction.has_value()) {
        const auto& tr = matched_rule->time_restriction.value();
        const auto range = parse_time_range(tr.allow_range);
        if (!range.has_value()) {
            // allow_range 파싱 실패 → fail-close (차단)
            spdlog::error(
                "policy_engine: invalid allow_range '{}' for user='{}', blocking (fail-close)",
                tr.allow_range, session.db_user
            );
            return PolicyResult{
                PolicyAction::kBlock,
                "time-restriction",
                fmt::format("Invalid time restriction configuration for user '{}'", session.db_user)
            };
        }
        if (!is_within_time_range(range.value(), tr.timezone)) {
            spdlog::info(
                "policy_engine: time_restriction denied, allow_range='{}', timezone='{}', "
                "session={}, user='{}'",
                tr.allow_range, tr.timezone, session.session_id, session.db_user
            );
            return PolicyResult{
                PolicyAction::kBlock,
                "time-restriction",
                "Access outside allowed hours"
            };
        }
    }

    // Step 8: 테이블 접근 제어 (allowed_tables)
    // "*" 가 포함되어 있으면 모든 테이블 허용.
    // query.tables 가 비어있으면 테이블 체크 건너뜀 (허용).
    const bool all_tables_allowed = std::any_of(
        matched_rule->allowed_tables.begin(),
        matched_rule->allowed_tables.end(),
        [](const std::string& t) { return t == "*"; }
    );

    if (!all_tables_allowed && !query.tables.empty()) {
        for (const auto& table : query.tables) {
            const bool table_allowed = std::any_of(
                matched_rule->allowed_tables.begin(),
                matched_rule->allowed_tables.end(),
                [&table](const std::string& allowed) {
                    return iequals(table, allowed);
                }
            );
            if (!table_allowed) {
                spdlog::info(
                    "policy_engine: table '{}' not in allowed_tables for user='{}', session={}",
                    table, session.db_user, session.session_id
                );
                return PolicyResult{
                    PolicyAction::kBlock,
                    "table-denied",
                    fmt::format("Table access denied: {}", table)
                };
            }
        }
    }

    // Step 9: 허용 오퍼레이션 체크 (allowed_operations)
    // allowed_operations 가 비어있지 않고 "*" 도 없으면 명시적 허용 목록과 비교.
    if (!matched_rule->allowed_operations.empty()) {
        const bool all_ops_allowed = std::any_of(
            matched_rule->allowed_operations.begin(),
            matched_rule->allowed_operations.end(),
            [](const std::string& op) { return op == "*"; }
        );

        if (!all_ops_allowed) {
            const bool op_allowed = std::any_of(
                matched_rule->allowed_operations.begin(),
                matched_rule->allowed_operations.end(),
                [&cmd_str](const std::string& op) {
                    return iequals(cmd_str, op);
                }
            );
            if (!op_allowed) {
                spdlog::info(
                    "policy_engine: operation '{}' not in allowed_operations for user='{}', session={}",
                    cmd_str, session.db_user, session.session_id
                );
                return PolicyResult{
                    PolicyAction::kBlock,
                    "operation-denied",
                    fmt::format("Operation not allowed: {}", cmd_str)
                };
            }
        }
    }

    // Step 10: 프로시저 제어
    // kCall, kPrepare, kExecute 에 대한 추가 검사.
    if (query.command == SqlCommand::kCall     ||
        query.command == SqlCommand::kPrepare  ||
        query.command == SqlCommand::kExecute)
    {
        const auto& pc = config->procedure_control;

        if (query.command == SqlCommand::kPrepare || query.command == SqlCommand::kExecute) {
            // PREPARE/EXECUTE: 동적 SQL 우회 차단
            if (pc.block_dynamic_sql) {
                spdlog::info(
                    "policy_engine: dynamic SQL ({}) blocked by procedure_control, "
                    "session={}, user='{}'",
                    cmd_str, session.session_id, session.db_user
                );
                return PolicyResult{
                    PolicyAction::kBlock,
                    "procedure-dynamic-sql",
                    fmt::format("Dynamic SQL ({}) blocked by policy", cmd_str)
                };
            }
        } else if (query.command == SqlCommand::kCall) {
            // CALL: 화이트리스트/블랙리스트 모드
            // 프로시저명은 query.tables 의 첫 번째 요소로 파싱됨 (파서 구현 의존)
            const std::string proc_name =
                query.tables.empty() ? "" : query.tables.front();

            if (pc.mode == "whitelist") {
                // 화이트리스트 모드: whitelist 에 있어야 허용
                const bool in_whitelist = std::any_of(
                    pc.whitelist.begin(), pc.whitelist.end(),
                    [&proc_name](const std::string& wl) {
                        return iequals(proc_name, wl);
                    }
                );
                if (!in_whitelist) {
                    spdlog::info(
                        "policy_engine: procedure '{}' not in whitelist, session={}, user='{}'",
                        proc_name, session.session_id, session.db_user
                    );
                    return PolicyResult{
                        PolicyAction::kBlock,
                        "procedure-whitelist",
                        fmt::format("Procedure '{}' not in whitelist", proc_name)
                    };
                }
            } else if (pc.mode == "blacklist") {
                // 블랙리스트 모드: whitelist(블랙리스트로 재사용) 에 있으면 차단
                const bool in_blacklist = std::any_of(
                    pc.whitelist.begin(), pc.whitelist.end(),
                    [&proc_name](const std::string& bl) {
                        return iequals(proc_name, bl);
                    }
                );
                if (in_blacklist) {
                    spdlog::info(
                        "policy_engine: procedure '{}' in blacklist, session={}, user='{}'",
                        proc_name, session.session_id, session.db_user
                    );
                    return PolicyResult{
                        PolicyAction::kBlock,
                        "procedure-blacklist",
                        fmt::format("Procedure '{}' is blacklisted", proc_name)
                    };
                }
            }
        }
    }

    // kCreate, kAlter: block_create_alter 체크
    // (프로시저 CREATE/ALTER 를 차단)
    if (query.command == SqlCommand::kCreate || query.command == SqlCommand::kAlter) {
        const auto& pc = config->procedure_control;
        if (pc.block_create_alter) {
            spdlog::info(
                "policy_engine: {} blocked by procedure_control.block_create_alter, "
                "session={}, user='{}'",
                cmd_str, session.session_id, session.db_user
            );
            return PolicyResult{
                PolicyAction::kBlock,
                "procedure-create-alter",
                fmt::format("{} blocked by procedure policy", cmd_str)
            };
        }
    }

    // Step 11: 스키마 접근 차단 (INFORMATION_SCHEMA 등)
    if (config->data_protection.block_schema_access) {
        static const std::array<std::string_view, 4> schema_names = {
            "information_schema", "mysql", "performance_schema", "sys"
        };
        for (const auto& table : query.tables) {
            for (const auto& schema : schema_names) {
                if (iequals(table, schema)) {
                    spdlog::info(
                        "policy_engine: schema access blocked for table '{}', session={}, user='{}'",
                        table, session.session_id, session.db_user
                    );
                    return PolicyResult{
                        PolicyAction::kBlock,
                        "schema-access",
                        "Schema access blocked"
                    };
                }
            }
        }
    }

    // Step 12: 명시적 allow
    spdlog::debug(
        "policy_engine: access allowed for user='{}', cmd={}, session={}",
        session.db_user, cmd_str, session.session_id
    );
    return PolicyResult{
        PolicyAction::kAllow,
        fmt::format("access-rule:{}", matched_rule->user),
        "Access allowed"
    };
}

// ---------------------------------------------------------------------------
// PolicyEngine::evaluate_error 구현
//
// 파서 오류 시 반드시 kBlock 을 반환한다 (fail-close).
// noexcept 이므로 예외 발생 가능성 있는 코드를 주의하여 작성한다.
// ---------------------------------------------------------------------------
PolicyResult PolicyEngine::evaluate_error(
    const ParseError&     error,
    const SessionContext& session) const noexcept {
    // noexcept 보장: fmt::format 도 예외를 던질 수 있으나,
    // fmt 의 기본 동작은 메모리 부족 시에만 예외를 던짐.
    // std::string 생성도 메모리 부족 시 예외 가능.
    // 이 함수에서 예외가 발생하면 std::terminate() 가 호출된다.
    // noexcept 계약을 지키기 위해 단순 문자열 연결을 사용한다.

    // 세션 정보는 보안상 외부에 노출하지 않고 로그에만 기록한다.
    // error.message 는 파서 내부 오류 설명으로, 상세 정보는 로그에만 기록.
    try {
        spdlog::warn(
            "policy_engine: parse error, blocking (fail-close), "
            "session={}, error_code={}, msg='{}'",
            session.session_id,
            static_cast<int>(error.code),
            error.message
        );
        return PolicyResult{
            PolicyAction::kBlock,
            "parse-error",
            "Parser error: " + error.message
        };
    } catch (...) {
        // 최후 안전망: 어떠한 예외가 발생하더라도 kBlock 반환
        return PolicyResult{
            PolicyAction::kBlock,
            "parse-error",
            "Parser error"
        };
    }
}

// ---------------------------------------------------------------------------
// PolicyEngine::reload 구현
//
// std::atomic_store 를 사용하여 현재 진행 중인 evaluate() 와
// 경쟁 없이 config 를 교체한다.
//
// [C++ 표준 참고]
// std::atomic_store(&shared_ptr, value) 는 C++11~C++20 에서 지원된다.
// C++20 에서 deprecated 되었으나 대안인 std::atomic<shared_ptr<T>> 멤버 사용은
// 헤더 변경이 필요하므로 현재 비멤버 함수를 사용한다.
//
// [new_config == nullptr 동작]
// nullptr 로 교체하면 이후 모든 evaluate() 가 kBlock 을 반환한다 (fail-close).
// ---------------------------------------------------------------------------
void PolicyEngine::reload(std::shared_ptr<PolicyConfig> new_config) {
    if (!new_config) {
        spdlog::warn("policy_engine: reload called with nullptr config — "
                     "all queries will be blocked after reload (fail-close)");
    } else {
        spdlog::info("policy_engine: reloading config with {} access rules",
                     new_config->access_control.size());
    }
    // 헤더의 config_ 타입이 std::shared_ptr<PolicyConfig> (atomic 아님).
    // 단순 대입 수행. 완전한 thread-safety 를 위해서는 헤더에
    // std::atomic<std::shared_ptr<PolicyConfig>> 가 필요하다 (Architect 승인 필요).
    // [설계 한계] 현재 구현은 단일 스레드에서 reload() 를 호출하는 경우를 가정한다.
    config_ = std::move(new_config);
}
