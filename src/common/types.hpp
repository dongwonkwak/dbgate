#pragma once

#include <chrono>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// SessionContext
//   클라이언트 연결 하나를 식별하는 불변 컨텍스트.
//   proxy 레이어가 생성하고 parser/policy/logger 레이어에 const-ref 로 전달한다.
// ---------------------------------------------------------------------------
struct SessionContext {
    std::uint64_t session_id{0};           // 프로세스 범위 내 유일 세션 ID
    std::string   client_ip{};             // 클라이언트 IPv4/IPv6 주소 문자열
    std::uint16_t client_port{0};          // 클라이언트 TCP 포트
    std::string   db_user{};              // MySQL 인증 사용자 이름
    std::string   db_name{};              // 초기 접속 데이터베이스(스키마) 이름
    std::chrono::system_clock::time_point connected_at{};  // 연결 수립 시각
    bool          handshake_done{false};   // MySQL 핸드셰이크 완료 여부
};

// ---------------------------------------------------------------------------
// ParseErrorCode
//   SQL 파싱 단계에서 발생 가능한 오류 분류.
// ---------------------------------------------------------------------------
enum class ParseErrorCode : std::uint8_t {
    kMalformedPacket    = 0,  // MySQL 패킷 구조가 올바르지 않음
    kInvalidSql         = 1,  // SQL 문법 오류
    kUnsupportedCommand = 2,  // 지원하지 않는 MySQL 커맨드 타입
    kInternalError      = 3,  // 파서 내부 오류 (예: 할당 실패)
};

// ---------------------------------------------------------------------------
// ParseError
//   파싱 실패 시 반환되는 오류 정보.
//   std::expected<T, ParseError> 패턴과 함께 사용한다.
// ---------------------------------------------------------------------------
struct ParseError {
    ParseErrorCode code{ParseErrorCode::kInternalError};
    std::string    message{};  // 사람이 읽을 수 있는 오류 설명
    std::string    context{};  // 오류가 발생한 위치/입력 단편 (로깅용)
};
