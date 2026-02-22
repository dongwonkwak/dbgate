// ---------------------------------------------------------------------------
// structured_logger.cpp
//
// StructuredLogger stub 구현.
// Phase 3 에서 spdlog JSON 직렬화로 교체 예정.
//
// [현재 상태]
// - 모든 log_* 메서드는 stub (미구현) 상태이다.
// - JSON 직렬화, 파일 로테이션, spdlog 초기화는 Phase 3 에서 구현한다.
// ---------------------------------------------------------------------------

#include "logger/structured_logger.hpp"

// StructuredLogger 생성자
//   Phase 3 에서 spdlog 인스턴스를 초기화하고 log_path 에 파일 핸들을 설정.
StructuredLogger::StructuredLogger(LogLevel min_level,
                                   const std::filesystem::path& log_path)
    : min_level_(min_level)
    , log_path_(log_path)
{
    // stub: Phase 3 에서 spdlog::rotating_logger_mt 또는 basic_logger_mt 초기화
}

// log_connection stub
//   TODO(Phase 3): ConnectionLog 를 JSON 으로 직렬화하여 기록
//   JSON 스키마 예시:
//   {
//     "type": "connection",
//     "session_id": <uint64>,
//     "event": "connect" | "disconnect",
//     "client_ip": <string>,
//     "client_port": <uint16>,
//     "db_user": <string>,
//     "timestamp": <ISO8601>
//   }
void StructuredLogger::log_connection([[maybe_unused]] const ConnectionLog& entry) {
    // stub: 미구현
}

// log_query stub
//   TODO(Phase 3): QueryLog 를 JSON 으로 직렬화하여 기록
//   [고빈도 호출 경로] JSON 직렬화 시 문자열 복사 최소화 검토 필요.
//   JSON 스키마 예시:
//   {
//     "type": "query",
//     "session_id": <uint64>,
//     "db_user": <string>,
//     "client_ip": <string>,
//     "raw_sql": <string>,  -- 마스킹 정책 적용 후 기록
//     "command_raw": <uint8>,
//     "tables": [<string>, ...],
//     "action_raw": <uint8>,
//     "timestamp": <ISO8601>,
//     "duration_us": <int64>
//   }
void StructuredLogger::log_query([[maybe_unused]] const QueryLog& entry) {
    // stub: 미구현
}

// log_block stub
//   TODO(Phase 3): BlockLog 를 JSON 으로 직렬화하여 기록
//   JSON 스키마 예시:
//   {
//     "type": "block",
//     "session_id": <uint64>,
//     "db_user": <string>,
//     "client_ip": <string>,
//     "raw_sql": <string>,  -- 마스킹 정책 적용 후 기록
//     "matched_rule": <string>,
//     "reason": <string>,
//     "timestamp": <ISO8601>
//   }
void StructuredLogger::log_block([[maybe_unused]] const BlockLog& entry) {
    // stub: 미구현
}

// 내부 진단용 spdlog 래퍼 stubs
//   TODO(Phase 3): spdlog::logger::debug/info/warn/error 호출로 교체

void StructuredLogger::debug([[maybe_unused]] std::string_view msg) {
    // stub: 미구현
}

void StructuredLogger::info([[maybe_unused]] std::string_view msg) {
    // stub: 미구현
}

void StructuredLogger::warn([[maybe_unused]] std::string_view msg) {
    // stub: 미구현
}

void StructuredLogger::error([[maybe_unused]] std::string_view msg) {
    // stub: 미구현
}
