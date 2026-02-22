# Architect Memory

## 프로젝트 현황 (Phase 2 시작 시점)
- Phase 1 완료: devcontainer, CMakeLists.txt, CMakePresets.json, ADR-001, ADR-003, policy.yaml, 디렉토리 골격
- Phase 2 진행 중 (feat/interfaces 브랜치): 헤더 인터페이스 확정 + ADR-002, ADR-005
- 현재 src/ 하위 모든 디렉토리는 .gitkeep만 존재 (구현 없음)

## 확정된 설계 원칙
- 핸드셰이크 패스스루: COM_QUERY 단계부터만 파싱/정책 적용
- 1:1 세션 릴레이: 커넥션 풀링 스코프 외
- SQL 파서 범위: 키워드 + 정규식 수준 (풀 파서 아님)
- Fail-close: 정책 엔진/파서 오류 시 항상 차단
- C++ ↔ Go: Unix Domain Socket 통신 필수

## 모듈 의존성 (순환 없음)
config -> (모든 모듈)
protocol -> (없음, 독립)
parser -> protocol (패킷에서 SQL 추출)
policy -> parser (ParsedQuery 소비), config
logger -> (없음, 독립)
stats -> logger
proxy -> protocol, parser, policy, logger, stats
health -> stats
tools(Go) -> stats/uds_server (UDS 소켓)

## 공통 타입 위치
- src/common/types.hpp: SessionContext, ParseError
- src/protocol/mysql_packet.hpp: MysqlPacket, PacketType
- src/parser/sql_parser.hpp: ParsedQuery, SqlCommand
- src/policy/policy_engine.hpp: PolicyResult, PolicyAction
- src/policy/rule.hpp: AccessRule, SqlRule, ProcedureControl
- src/logger/log_types.hpp: LogEntry, LogLevel
- src/stats/stats_collector.hpp: StatsSnapshot

## 인터페이스 변경 관리
- 서브에이전트는 인터페이스 제안만 가능, Architect 승인 후 반영
- 변경 요청 시: 이유 + 영향 모듈 + 테스트 영향 포함 필수

## Phase별 ADR 배치
- Phase 2: ADR-002 (handshake-passthrough), ADR-005 (cpp-go-language-split)
- Phase 3: ADR-004 (yaml-policy-format), ADR-006 (sql-parser-scope)

## 주의사항
- CMakeLists.txt: 새 파일 추가 시 반드시 업데이트 (서브에이전트 책임)
- 전역 변수 금지, raw epoll 금지, using namespace std 금지
- 하드코딩 포트/경로 금지

## 세부 설계 파일 링크
- [인터페이스 설계](./interfaces.md)
