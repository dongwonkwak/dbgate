#pragma once

// ---------------------------------------------------------------------------
// rule.hpp
//
// 정책 설정 구조체 정의 (헤더만, 구현 없음).
// yaml-cpp 를 통해 config/policy.yaml 에서 로드된다.
//
// [설계 원칙]
// - 이 헤더는 다른 헤더에 의존하지 않는다 (독립적).
// - 모든 컨테이너 멤버는 기본값을 명시하여 미초기화 동작을 방지한다.
// - 정책 평가 실패 시 항상 kBlock (fail-close). 이 구조체 자체는
//   판정 로직을 포함하지 않는다.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// TimeRestriction
//   시간대별 접근 제어 설정.
//   allow_range 형식: "HH:MM-HH:MM" (예: "09:00-18:00")
//   timezone 형식: IANA timezone ID (예: "Asia/Seoul")
//
//   [한계]
//   - timezone 파싱은 구현 레이어에서 OS의 tzdata 에 의존한다.
//   - 서머타임(DST) 전환 경계에서 1시간 오차 가능성 있음.
// ---------------------------------------------------------------------------
struct TimeRestriction {
    std::string allow_range{"09:00-18:00"};  // 접근 허용 시간 범위
    std::string timezone{"UTC"};             // IANA timezone ID
};

// ---------------------------------------------------------------------------
// AccessRule
//   사용자/IP 기반 접근 제어 규칙.
//   allowed_tables = ["*"] 이면 모든 테이블 접근 허용.
//   blocked_operations 가 allowed_operations 보다 우선 적용된다.
//
//   [source_ip_cidr]
//   CIDR 표기법 사용 (예: "192.168.1.0/24", "10.0.0.1/32").
//   빈 문자열이면 모든 IP 허용으로 해석한다.
//   [보안 주의] IP 스푸핑 방어는 네트워크 레이어 소관.
// ---------------------------------------------------------------------------
struct AccessRule {
    std::string              user{};                // MySQL 사용자 이름 ("*" = 와일드카드)
    std::string              source_ip_cidr{};      // 허용 소스 IP CIDR (빈값 = 전체)
    std::vector<std::string> allowed_tables{"*"};   // 접근 허용 테이블 목록
    std::vector<std::string> allowed_operations{};  // 허용 SQL 커맨드 목록
    std::vector<std::string> blocked_operations{};  // 차단 SQL 커맨드 목록 (우선)

    // 시간대 제한 (설정 없으면 24시간 허용)
    std::optional<TimeRestriction> time_restriction{};
};

// ---------------------------------------------------------------------------
// SqlRule
//   SQL 구문 레벨 차단 규칙.
//   block_statements: SQL 커맨드 문자열 (예: ["DROP", "TRUNCATE"])
//   block_patterns:   정규식 패턴 (InjectionDetector 와 공유)
// ---------------------------------------------------------------------------
struct SqlRule {
    std::vector<std::string> block_statements{};  // 차단할 SQL 구문 종류
    std::vector<std::string> block_patterns{};    // 정규식 기반 차단 패턴
};

// ---------------------------------------------------------------------------
// ProcedureControl
//   프로시저 화이트리스트/블랙리스트 설정.
//   mode = "whitelist": whitelist 에 없는 프로시저는 차단.
//   mode = "blacklist": whitelist 에 있는 프로시저는 차단. (필드명 재사용)
//
//   [보안 원칙]
//   - block_dynamic_sql = true 권장: PREPARE/EXECUTE 기반 우회 차단.
//   - block_create_alter = true 권장: 스키마 변경 차단.
//   - mode 가 "whitelist" 이고 whitelist 가 비어 있으면 모든 CALL 차단.
// ---------------------------------------------------------------------------
struct ProcedureControl {
    std::string              mode{"whitelist"};      // "whitelist" | "blacklist"
    std::vector<std::string> whitelist{};            // 허용/차단 프로시저 목록
    bool                     block_dynamic_sql{true};   // PREPARE/EXECUTE 차단
    bool                     block_create_alter{true};  // CREATE/ALTER PROCEDURE 차단
};

// ---------------------------------------------------------------------------
// DataProtection
//   결과 행 수 제한 및 스키마 접근 차단 설정.
//   max_result_rows = 0 이면 제한 없음.
//   block_schema_access = true 이면 information_schema, mysql DB 접근 차단.
// ---------------------------------------------------------------------------
struct DataProtection {
    std::uint32_t max_result_rows{0};           // 0 = 제한 없음
    bool          block_schema_access{true};    // 스키마 메타데이터 접근 차단
};

// ---------------------------------------------------------------------------
// GlobalConfig
//   전역 설정값.
//   log_level: "trace"|"debug"|"info"|"warn"|"error"|"critical"
//   log_format: "json" | "text"
// ---------------------------------------------------------------------------
struct GlobalConfig {
    std::string   log_level{"info"};
    std::string   log_format{"json"};
    std::uint32_t max_connections{1000};
    std::uint32_t connection_timeout_sec{30};
};

// ---------------------------------------------------------------------------
// PolicyConfig
//   전체 정책 설정의 루트 구조체.
//   PolicyLoader::load 가 반환하는 최종 결과물.
//   PolicyEngine 이 평가 시 참조한다.
//
//   [Hot Reload 고려사항]
//   - PolicyEngine::reload 를 통해 shared_ptr 교체 방식으로 원자적 갱신.
//   - 갱신 중 평가가 진행 중인 경우 이전 config 로 완료됨 (eventual consistency).
// ---------------------------------------------------------------------------
struct PolicyConfig {
    GlobalConfig             global{};
    std::vector<AccessRule>  access_control{};
    SqlRule                  sql_rules{};
    ProcedureControl         procedure_control{};
    DataProtection           data_protection{};
};
