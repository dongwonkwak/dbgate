#pragma once

#include "common/types.hpp"
#include "protocol/mysql_packet.hpp"

#include <cstdint>
#include <expected>
#include <string>

// ---------------------------------------------------------------------------
// CommandType
//   MySQL COM_* 커맨드 바이트 값을 열거한다.
//   핸드셰이크 완료 이후의 커맨드 패킷 첫 바이트에 대응한다.
// ---------------------------------------------------------------------------
enum class CommandType : std::uint8_t {
    kComQuit        = 0x01,  // COM_QUIT
    kComInitDb      = 0x02,  // COM_INIT_DB  (USE database)
    kComQuery       = 0x03,  // COM_QUERY    (SQL 문 실행)
    kComFieldList   = 0x04,  // COM_FIELD_LIST
    kComCreateDb    = 0x05,  // COM_CREATE_DB
    kComDropDb      = 0x06,  // COM_DROP_DB
    kComRefresh     = 0x07,  // COM_REFRESH
    kComStatistics  = 0x09,  // COM_STATISTICS
    kComProcessInfo = 0x0A,  // COM_PROCESS_INFO
    kComConnect     = 0x0B,  // COM_CONNECT
    kComProcessKill = 0x0C,  // COM_PROCESS_KILL
    kComPing        = 0x0E,  // COM_PING
    kComStmtPrepare = 0x16,  // COM_STMT_PREPARE
    kComStmtExecute = 0x17,  // COM_STMT_EXECUTE
    kComStmtClose   = 0x19,  // COM_STMT_CLOSE
    kComStmtReset   = 0x1A,  // COM_STMT_RESET
    kComUnknown     = 0xFF,  // 미분류 / 파싱 불가
};

// ---------------------------------------------------------------------------
// CommandPacket
//   핸드셰이크 이후 클라이언트가 보내는 단일 커맨드를 나타낸다.
//
//   command_type : COM_* 종류
//   query        : COM_QUERY 의 경우 SQL 문자열 (다른 커맨드는 빈 문자열)
//   sequence_id  : MySQL 패킷 시퀀스 번호 (응답 생성 시 +1)
// ---------------------------------------------------------------------------
struct CommandPacket {
    CommandType  command_type{CommandType::kComUnknown};
    std::string  query{};        // COM_QUERY payload (UTF-8 SQL)
    std::uint8_t sequence_id{0};
};

// ---------------------------------------------------------------------------
// extract_command
//   MysqlPacket 에서 CommandPacket 을 추출한다.
//
//   packet 의 페이로드 첫 바이트를 CommandType 으로 해석하며,
//   COM_QUERY 이면 나머지 바이트를 query 문자열로 설정한다.
//
//   실패 조건:
//     - 페이로드가 비어 있음  -> ParseErrorCode::kMalformedPacket
//     - 지원하지 않는 커맨드 -> ParseErrorCode::kUnsupportedCommand
// ---------------------------------------------------------------------------
auto extract_command(const MysqlPacket& packet)
    -> std::expected<CommandPacket, ParseError>;
