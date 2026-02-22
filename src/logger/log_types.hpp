#pragma once

// ---------------------------------------------------------------------------
// log_types.hpp
//
// 로거 서브시스템에서 사용하는 구조화 로그 타입 정의.
//
// [순환 의존성 방지 설계]
// - SqlCommand, PolicyAction 을 직접 include 하지 않는다.
// - command_raw (uint8_t): 호출자가 static_cast<uint8_t>(SqlCommand) 로 변환
// - action_raw  (uint8_t): 호출자가 static_cast<uint8_t>(PolicyAction) 로 변환
//
// [민감정보 취급 주의]
// - raw_sql 은 원문 SQL 전체를 포함한다. 운영 환경에서 로그 레벨/마스킹
//   정책을 별도로 적용할 것.
// ---------------------------------------------------------------------------

#include "common/types.hpp"  // std::uint64_t 등 기본 타입 (SessionContext 불포함)

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// LogLevel
//   로거의 최소 출력 레벨. config 에서 주입.
// ---------------------------------------------------------------------------
enum class LogLevel : std::uint8_t {
    kDebug = 0,
    kInfo  = 1,
    kWarn  = 2,
    kError = 3,
};

// ---------------------------------------------------------------------------
// ConnectionLog
//   클라이언트 연결/해제 이벤트 로그.
//   event: "connect" | "disconnect"
// ---------------------------------------------------------------------------
struct ConnectionLog {
    std::uint64_t                              session_id{0};
    std::string                                event{};        // "connect" | "disconnect"
    std::string                                client_ip{};
    std::uint16_t                              client_port{0};
    std::string                                db_user{};
    std::chrono::system_clock::time_point      timestamp{};
};

// ---------------------------------------------------------------------------
// QueryLog
//   SQL 쿼리 실행 로그.
//
//   command_raw: SqlCommand 값을 uint8_t 로 저장 (include 없이 캐스팅)
//     호출자: static_cast<uint8_t>(parsed_query.command)
//
//   action_raw: PolicyAction 값을 uint8_t 로 저장 (include 없이 캐스팅)
//     호출자: static_cast<uint8_t>(policy_result.action)
// ---------------------------------------------------------------------------
struct QueryLog {
    std::uint64_t                              session_id{0};
    std::string                                db_user{};
    std::string                                client_ip{};
    std::string                                raw_sql{};      // 원문 SQL (마스킹 주의)
    std::uint8_t                               command_raw{0}; // SqlCommand as uint8_t
    std::vector<std::string>                   tables{};       // 접근 테이블명 목록
    std::uint8_t                               action_raw{0};  // PolicyAction as uint8_t
    std::chrono::system_clock::time_point      timestamp{};
    std::chrono::microseconds                  duration{0};    // 정책 평가 소요 시간
};

// ---------------------------------------------------------------------------
// BlockLog
//   차단 이벤트 로그.
//   matched_rule: 매칭된 규칙 식별자 ("default-deny" 포함)
//   reason: 사람이 읽을 수 있는 차단 사유 (클라이언트에 직접 노출 금지)
// ---------------------------------------------------------------------------
struct BlockLog {
    std::uint64_t                              session_id{0};
    std::string                                db_user{};
    std::string                                client_ip{};
    std::string                                raw_sql{};      // 원문 SQL (마스킹 주의)
    std::string                                matched_rule{}; // 매칭 규칙 ID
    std::string                                reason{};       // 차단 사유
    std::chrono::system_clock::time_point      timestamp{};
};
