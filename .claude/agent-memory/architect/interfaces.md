# Phase 2 인터페이스 확정 기록

## 확정일: 2026-02-22 (DON-23)

## 공통 타입: src/common/types.hpp
- SessionContext: session_id, client_ip, client_port, db_user, db_name, connected_at, handshake_done
- ParseError: code(enum), message, context
- ParseErrorCode: kMalformedPacket, kInvalidSql, kUnsupportedCommand, kInternalError

## protocol 헤더
- src/protocol/mysql_packet.hpp: MysqlPacket, PacketType, parse()
- src/protocol/handshake.hpp: HandshakeRelay, relay_handshake()
- src/protocol/command.hpp: CommandPacket, CommandType, extract_query()

## parser 헤더
- src/parser/sql_parser.hpp: ParsedQuery, SqlCommand, SqlParser
- src/parser/procedure_detector.hpp: ProcedureInfo, ProcedureType, ProcedureDetector
- src/parser/injection_detector.hpp: InjectionResult, InjectionDetector

## policy 헤더
- src/policy/rule.hpp: AccessRule, SqlRule, ProcedureControl, DataProtection, GlobalConfig, PolicyConfig
- src/policy/policy_engine.hpp: PolicyAction, PolicyResult, PolicyEngine
- src/policy/policy_loader.hpp: PolicyLoader

## logger 헤더
- src/logger/log_types.hpp: LogLevel, ConnectionLog, QueryLog, BlockLog
- src/logger/structured_logger.hpp: StructuredLogger

## stats 헤더
- src/stats/stats_collector.hpp: StatsSnapshot, StatsCollector
- src/stats/uds_server.hpp: UdsServer

## health 헤더
- src/health/health_check.hpp: HealthStatus, HealthCheck

## proxy 헤더
- src/proxy/session.hpp: SessionState, Session
- src/proxy/proxy_server.hpp: ProxyConfig, ProxyServer
