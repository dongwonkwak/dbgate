#pragma once

// ---------------------------------------------------------------------------
// structured_logger.hpp
//
// spdlog 기반 구조화 JSON 로거 인터페이스.
//
// [설계 원칙]
// - 싱글턴 금지: 생성자 주입 방식으로 의존성을 명시적으로 표현한다.
// - 고빈도 로그 경로(log_query)에서 불필요한 문자열 복사를 줄이기 위해
//   const-ref 파라미터를 사용한다.
// - 민감정보(raw_sql 등)의 실제 마스킹 정책은 Phase 3 구현에서 적용한다.
//
// [JSON 스키마 일관성]
// 파서/정책/프록시 로그 간 필드명 불일치를 최소화하기 위해
// 모든 구조체 필드를 snake_case JSON 키로 직렬화한다.
// ---------------------------------------------------------------------------

#include "log_types.hpp"

#include <filesystem>
#include <string_view>

// ---------------------------------------------------------------------------
// StructuredLogger
//   ConnectionLog / QueryLog / BlockLog 를 JSON 포맷으로 기록한다.
//   내부 진단용 debug/info/warn/error 메서드도 제공한다.
// ---------------------------------------------------------------------------
class StructuredLogger {
public:
    // 생성자
    //   min_level : 이 레벨 미만의 로그는 기록하지 않는다.
    //   log_path  : 로그 파일 경로 (디렉터리가 아닌 파일 경로)
    explicit StructuredLogger(LogLevel min_level,
                              const std::filesystem::path& log_path);

    ~StructuredLogger() = default;

    // 복사 금지 (spdlog 인스턴스 소유권 명확화)
    StructuredLogger(const StructuredLogger&)            = delete;
    StructuredLogger& operator=(const StructuredLogger&) = delete;

    // 이동 허용
    StructuredLogger(StructuredLogger&&)            = default;
    StructuredLogger& operator=(StructuredLogger&&) = default;

    // log_connection
    //   클라이언트 연결/해제 이벤트를 JSON 으로 기록한다.
    void log_connection(const ConnectionLog& entry);

    // log_query
    //   SQL 쿼리 실행 결과를 JSON 으로 기록한다.
    //   [고빈도 호출 경로] 불필요한 문자열 복사를 최소화할 것.
    void log_query(const QueryLog& entry);

    // log_block
    //   차단 이벤트를 JSON 으로 기록한다.
    void log_block(const BlockLog& entry);

    // 내부 진단용 spdlog 래퍼
    //   프록시/정책/파서 내부 상태를 기록할 때 사용한다.
    //   클라이언트 데이터(SQL, 사용자명 등)를 직접 전달하지 말 것.
    void debug(std::string_view msg);
    void info(std::string_view msg);
    void warn(std::string_view msg);
    void error(std::string_view msg);

private:
    LogLevel              min_level_;
    std::filesystem::path log_path_;
    // Phase 3 구현: spdlog::logger 인스턴스를 shared_ptr 로 보유
    // std::shared_ptr<spdlog::logger> logger_;
};
