# dbgate 아키텍처 설명서

## 개요

dbgate는 MySQL 클라이언트와 MySQL 서버 사이에 위치하는 **DB 접근제어 프록시**입니다. 높은 성능이 요구되는 **데이터패스(data path)**는 C++로, 운영 편의성이 중요한 **컨트롤플레인(control plane)**은 Go로 구현되어 있습니다.

## 모듈 구조 다이어그램

```
┌─────────────────────────────────────────────────────────────────────┐
│                          dbgate Instance                             │
├─────────────────────────────────────────────────────────────────────┤
│                                                                       │
│  ┌─────────────┐                                    ┌──────────────┐ │
│  │ DB Client   │                                    │ MySQL Server │ │
│  └──────┬──────┘                                    └──────────────┘ │
│         │                                                    ▲        │
│         │ TCP Connect (Port 13306)                         │        │
│         │                                                   │        │
│         ▼                                                   │        │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  Boost.Asio (epoll 기반 비동기 I/O, C++23 코루틴)             │  │
│  │  - TCP Accept                                                 │  │
│  │  - Session Manager (1:1 릴레이)                              │  │
│  │  - Strand (스레드 안전성)                                     │  │
│  └───────────────────────────────────────────────────────────────┘  │
│         ▲                                                            │
│         │                                                            │
│  ┌──────┴──────────────────────────────────────────────────────┐   │
│  │          핸드셰이크 단계 (투명 패스스루)                        │   │
│  │  - 클라이언트 ← → 서버 간 패킷 투명 릴레이                   │   │
│  │  - auth plugin 개입 없음                                      │   │
│  │  - SessionContext 구성 (db_user, db_name 추출)               │   │
│  └──────┬───────────────────────────────────────────────────────┘   │
│         │                                                            │
│  ┌──────▼──────────────────────────────────────────────────────┐   │
│  │             COM_QUERY 처리 루프 (정책 적용)                   │   │
│  │  1. MySQL 패킷 수신 (parse)                                 │   │
│  │  2. SQL 추출                                                 │   │
│  │        │                                                     │   │
│  │  ┌─────▼──────────────────────────────────────────────┐    │   │
│  │  │            SQL Parser (경량)                       │    │   │
│  │  │  - 첫 번째 키워드로 구문 분류                        │    │   │
│  │  │    (SELECT/INSERT/UPDATE/DELETE/DROP/...)         │    │   │
│  │  │  - 테이블명 추출 (기본적)                           │    │   │
│  │  │  - 정규식 기반 간단한 패턴 매칭                    │    │   │
│  │  │  ParsedQuery 생성                                 │    │   │
│  │  └─────┬──────────────────────────────────────────────┘    │   │
│  │        │                                                     │   │
│  │  ┌─────▼──────────────────────────────────────────────┐    │   │
│  │  │      InjectionDetector (정규식)                    │    │   │
│  │  │  - SQL Injection 패턴 탐지                          │    │   │
│  │  │  - 주석 분할, 인코딩 우회 등 한계 존재              │    │   │
│  │  └─────┬──────────────────────────────────────────────┘    │   │
│  │        │                                                     │   │
│  │  ┌─────▼──────────────────────────────────────────────┐    │   │
│  │  │      ProcedureDetector (상태 분류)                 │    │   │
│  │  │  - CALL/PREPARE/EXECUTE 탐지                       │    │   │
│  │  │  - 동적 SQL 우회 가능성 표시                        │    │   │
│  │  └─────┬──────────────────────────────────────────────┘    │   │
│  │        │                                                     │   │
│  │  ┌─────▼──────────────────────────────────────────────┐    │   │
│  │  │         PolicyEngine (정책 판정)                   │    │   │
│  │  │  1. SQL 구문 차단 규칙 확인                         │    │   │
│  │  │  2. 패턴 기반 차단 확인                             │    │   │
│  │  │  3. 사용자/IP 접근 제어                            │    │   │
│  │  │  4. 테이블 접근 제어                                │    │   │
│  │  │  5. 시간대 제한                                     │    │   │
│  │  │  6. 프로시저 제어 (화이트/블랙리스트)              │    │   │
│  │  │  7. ALLOW / BLOCK / LOG 판정                       │    │   │
│  │  │                                                     │    │   │
│  │  │  원칙: ALLOW는 명시적 허용 규칙 필요                │    │   │
│  │  │        일치 없음 → BLOCK (default deny)             │    │   │
│  │  │        파싱 오류 → BLOCK (fail-close)               │    │   │
│  │  └─────┬──────────────────────────────────────────────┘    │   │
│  │        │                                                     │   │
│  │  ┌─────▼─────────────────┬────────────────────────────┐    │   │
│  │  │                       │                            │    │   │
│  │  ▼                       ▼                            ▼    │   │
│  │[ALLOW]          [BLOCK/LOG]              [Internal Error]│   │
│  │  │                 │                         │         │   │
│  │  │    ┌────────────┴─────────────┐          │         │   │
│  │  │    ▼                          ▼          ▼         │   │
│  │  │  [릴레이]             [Error Response]  [Log]      │   │
│  │  │                                                     │   │
│  │  ▼                                                     │   │
│  └──────────────────────────────────────────────────────────┘   │
│         │                                                            │
│         ▼                                                            │
│  ┌──────────────────────────────────────────────────────┐           │
│  │          StructuredLogger (spdlog, JSON)            │           │
│  │  - 연결 로그 (connect/disconnect)                    │           │
│  │  - 쿼리 로그 (정책 판정 결과 포함)                   │           │
│  │  - 차단 로그 (차단 사유/규칙 정보)                   │           │
│  └──────────────────────────────────────────────────────┘           │
│         │                                                            │
│         ▼                                                            │
│  ┌──────────────────────────────────────────────────────┐           │
│  │          StatsCollector (실시간 통계, atomic)       │           │
│  │  - 총 연결 수 (total_connections)                    │           │
│  │  - 활성 세션 수 (active_sessions)                    │           │
│  │  - 총 쿼리 수 (total_queries)                        │           │
│  │  - 차단 쿼리 수 (blocked_queries)                    │           │
│  │  - 초당 쿼리 수 (QPS, 슬라이딩 윈도우)               │           │
│  │  - 차단 비율 (block_rate)                            │           │
│  └──────────────────────────────────────────────────────┘           │
│         │                                                            │
│         ▼                                                            │
│  ┌──────────────────────────────────────────────────────┐           │
│  │  UdsServer (Unix Domain Socket)                      │           │
│  │  - Go CLI 도구와 통신 (저오버헤드)                    │           │
│  │  - 프로토콜: 4byte LE 길이 + JSON 페이로드           │           │
│  │  - 지원 커맨드: stats / sessions / policy_reload     │           │
│  └──────────────────────────────────────────────────────┘           │
│                                                                       │
└─────────────────────────────────────────────────────────────────────┘
        │
        │ Unix Domain Socket (/tmp/dbgate.sock 등)
        │
        ▼
┌───────────────────────────────────┐
│      Go CLI / 웹 대시보드          │
│  (tools/cmd/dbgate-cli)           │
│  - sessions 조회                   │
│  - stats 조회                      │
│  - policy reload                   │
│  - 실시간 모니터링                 │
└───────────────────────────────────┘
```

## 모듈 의존 관계

```
common/
  ├─ types.hpp (공통 타입, enum, 구조체)
  │
protocol/ (← common)
  ├─ mysql_packet.hpp (MySQL 와이어 프로토콜 패킷)
  ├─ handshake.hpp    (핸드셰이크 패스스루)
  └─ command.hpp      (COM_QUERY 등 커맨드)
  │
parser/ (← common, protocol)
  ├─ sql_parser.hpp          (SQL 구문 분류)
  ├─ injection_detector.hpp  (Injection 탐지)
  └─ procedure_detector.hpp  (프로시저 탐지)
  │
policy/ (← common, parser)
  ├─ rule.hpp         (정책 구조체)
  ├─ policy_loader.hpp (YAML 로드)
  └─ policy_engine.hpp (정책 판정)
  │
logger/ (← common)
  ├─ log_types.hpp       (로그 구조체)
  └─ structured_logger.hpp (spdlog 래퍼)
  │
stats/ (← common)
  ├─ stats_collector.hpp  (실시간 통계)
  └─ uds_server.hpp       (UDS 서버)
  │
health/ (← common, stats)
  └─ health_check.hpp (HTTP Health Check)
  │
proxy/ (← common, protocol, parser, policy, logger, stats, health)
  ├─ proxy_server.hpp (TCP 서버)
  └─ session.hpp      (1:1 릴레이 세션)

✓ 무순환 (DAG) 구조 보장
✓ proxy 레이어만 모든 모듈에 의존 (통합점)
✓ logger/stats는 상위 모듈에 역의존하지 않음
```

## 데이터 흐름 상세

### 1단계: TCP 연결 수립

```
[DB Client] → TCP SYN → ProxyServer → Accept
                           │
                      Session 생성
                       (strand 할당)
```

### 2단계: MySQL 핸드셰이크

```
[Client]                              [Server]
   │                                     │
   │─── HandshakeInit Packet ────────────→
   │                                     │
   │←─ HandshakeInit Packet ─────────────│
   │                                     │
   │─ HandshakeResponse Packet (user, db) →
   │                                     │
   │←─ OK/Error ─────────────────────────│
```

**HandshakeRelay의 역할:**
- 클라이언트 ↔ 서버 간 패킷을 투명하게 릴레이
- auth plugin 메커니즘 개입 없음 (호환성 최대화)
- 핸드셰이크 완료 후 SessionContext 채우기:
  - `db_user`: HandshakeResponse에서 추출
  - `db_name`: 초기 접속 DB 이름
  - `handshake_done`: true로 마킹

### 3단계: COM_QUERY 처리 루프

```
While (세션 활성):
    1. MysqlPacket 수신 (크기 헤더 + 페이로드)
       │
    2. extract_command(packet)
       │
       ├─ COM_QUERY 아니면 → 그냥 릴레이 후 응답
       │
       ▼ COM_QUERY인 경우

    3. SQL Parser
       - sql_parser.parse(query_string)
       - ParsedQuery 반환 (command, tables, raw_sql)

    4. Injection Detection (병렬 가능)
       - InjectionDetector::check(raw_sql)
       - InjectionResult (detected=true/false)

    5. Procedure Detection (병렬 가능)
       - ProcedureDetector::detect(parsed_query)
       - ProcedureInfo (is_dynamic_sql 등)

    6. Policy Engine 평가
       - PolicyEngine::evaluate(parsed_query, session_context)
       - 순서: 구문 차단 → 패턴 차단 → 사용자/IP → 테이블 →
               시간대 → 프로시저 → 명시적 allow/default deny
       - PolicyResult (action=ALLOW/BLOCK/LOG)

    7-1. ALLOW인 경우
         └─ MySQL 서버로 릴레이
            └─ 응답을 클라이언트로 릴레이

    7-2. BLOCK인 경우
         └─ Error Packet 생성 (MySQL 호환)
         └─ 클라이언트에 오류 응답

    7-3. LOG인 경우
         └─ ALLOW와 동일하게 처리
         └─ 감사 로그에 기록

    8. 로깅 (모든 경로)
       - StructuredLogger::log_query(entry)
       - JSON 포맷으로 기록

    9. 통계 갱신 (모든 경로)
       - StatsCollector::on_query(blocked)
       - Atomic으로 카운터 증가
```

## Fail-Close 원칙 (절대 위반 금지)

**Fail-Close의 의미:**
> 불확실한 상황에서 항상 차단(BLOCK)을 선택하는 보안 원칙.

### Fail-Close가 필수인 경우

#### 1. 파서 오류 발생

```cpp
auto parsed = sql_parser.parse(query);
if (!parsed) {
    // 파싱 실패 → 정책 엔진에 명시적으로 알림
    auto result = policy_engine.evaluate_error(parsed.error(), ctx);
    // 반드시 PolicyAction::kBlock을 반환 (fail-close)
}
```

**이유:**
- 공격자가 파서가 이해하지 못하는 복잡한 SQL을 작성했을 수 있음
- 파서 우회 공격 방지 (예: DROP/**/TABLE)

#### 2. 정책 엔진 내부 오류

```cpp
PolicyResult evaluate() {
    // 설정 로드 실패? → BLOCK
    if (!config) return PolicyResult{.action = PolicyAction::kBlock};

    // 규칙 평가 중 오류? → BLOCK
    if (unexpected_error) return PolicyResult{.action = PolicyAction::kBlock};
}
```

#### 3. 정책 일치 없음

```cpp
// 기본값은 kBlock (default deny)
PolicyResult result{.action = PolicyAction::kBlock};

// 명시적 allow 규칙이 있을 때만 kAllow
if (matches_allow_rule(query, ctx)) {
    result.action = PolicyAction::kAllow;
}

return result;  // 일치 없으면 kBlock
```

#### 4. 정책 로드 실패

```cpp
auto loaded = PolicyLoader::load(config_path);
if (!loaded) {
    // 기존 정책 유지하거나 서비스 차단
    // fail-open은 절대 금지
    logger.error("Policy load failed: {}", loaded.error());
    // ProxyServer는 모든 쿼리를 차단하거나 기존 정책으로 계속
}
```

### 절대 금지하는 패턴

```cpp
// ❌ WRONG: 파서 오류 → ALLOW
if (!parsed) {
    return PolicyResult{.action = PolicyAction::kAllow};  // fail-open!
}

// ❌ WRONG: 설정 없음 → ALLOW
if (!config) {
    return PolicyResult{.action = PolicyAction::kAllow};  // fail-open!
}

// ❌ WRONG: 규칙 일치 없음 → ALLOW (whitelist 누락)
if (no_match) {
    return PolicyResult{.action = PolicyAction::kAllow};  // default whitelist!
}

// ❌ WRONG: 예외 발생 → ALLOW
try {
    // ...
} catch (...) {
    return PolicyResult{.action = PolicyAction::kAllow};  // fail-open!
}
```

## 모듈 구현 상태 (DON-28 기준)

| 모듈 | 상태 | 구현 범위 |
|------|------|----------|
| `common/` | ✓ 완료 | 공통 타입, enum, 구조체 정의 |
| `protocol/` | ✓ 완료 | MySQL 패킷 파싱, 핸드셰이크 패스스루 |
| `parser/` | ✓ 완료 | SQL 파싱, Injection 탐지, 프로시저 탐지 |
| `policy/` | ✓ 완료 | 정책 엔진, YAML 로더, Hot Reload |
| `logger/` | ✓ 완료 | 구조화 JSON 로깅 |
| `stats/` | ✓ 완료 | Atomic 통계 수집, UDS 서버 (JSON API) |
| `health/` | ✓ 완료 | HTTP /health 엔드포인트 (read-only stats) |
| `proxy/` | ✓ 완료 | 전체 통합: ProxyServer + Session (MySQL 프로토콜 파이프라인) |
| `main.cpp` | ✓ 완료 | 환경변수 기반 설정, signal 핸들링, graceful shutdown |

## 각 모듈의 책임

### common/types.hpp
- **책임**: 프로젝트 전역 타입, enum, 구조체 정의
- **주요 타입**:
  - `SessionContext`: 클라이언트 연결 정보 (불변)
  - `ParseErrorCode`, `ParseError`: 파싱 오류 정보
- **특징**: 다른 모듈에 의존하지 않는 독립적 헤더

### protocol 모듈
- **책임**: MySQL 와이어 프로토콜 처리
- **구성**:
  - `mysql_packet.hpp`: 패킷 파싱/직렬화
  - `handshake.hpp`: 핸드셰이크 투명 릴레이
  - `command.hpp`: COM_QUERY 추출
- **특징**:
  - 프로토콜 세부사항만 담당
  - SQL 해석은 하지 않음 (parser 책임)

### parser 모듈
- **책임**: SQL 구문 분석 및 보안 검사
- **구성**:
  - `sql_parser.hpp`: 구문 분류 (키워드 + 정규식)
  - `injection_detector.hpp`: Injection 패턴 탐지
  - `procedure_detector.hpp`: 프로시저/동적 SQL 탐지
- **특징**:
  - 경량 파서 (풀 파서 아님)
  - 한계를 문서화하고 명시 (주석 분할, 인코딩 우회 등)
  - 정책 엔진과 독립적 (필요한 정보만 제공)

### policy 모듈
- **책임**: 정책 파일 로드 및 쿼리 판정
- **구성**:
  - `rule.hpp`: 정책 데이터 구조 (YAML 매핑)
  - `policy_loader.hpp`: YAML 파일 로드 (+ Hot Reload)
  - `policy_engine.hpp`: 규칙 평가 및 ALLOW/BLOCK 판정
- **특징**:
  - Fail-close 원칙 엄격히 준수
  - Hot Reload 지원 (shared_ptr 교체)
  - 규칙 평가 순서 명확화

### logger 모듈
- **책임**: 구조화 JSON 로깅
- **구성**:
  - `log_types.hpp`: 로그 구조체 (ConnectionLog, QueryLog, BlockLog)
  - `structured_logger.hpp`: spdlog 래퍼
- **특징**:
  - 민감정보(raw_sql) 취급 주의
  - 로깅 실패가 데이터패스로 전파되지 않도록 설계
  - JSON 스키마 일관성 유지

### stats 모듈
- **책임**: 실시간 통계 수집 및 UDS 기반 조회 API
- **구성**:
  - `stats_collector.hpp`: Atomic 기반 통계 (mutex 없음)
  - `uds_server.hpp`: Unix Domain Socket 서버 (JSON 프레임 기반)
- **특징**:
  - 고성능 (lock-free atomic)
  - 데이터패스 오버헤드 최소화
  - Go CLI와 저레이턴시 통신
  - 지원 커맨드: `stats`, `sessions`, `policy_reload`
  - 프로토콜: 4byte LE 길이 + JSON 페이로드

### health 모듈
- **책임**: HTTP 헬스체크 엔드포인트 + stats 조회
- **구성**:
  - `health_check.hpp`: HTTP/1.0 기반 `/health` 엔드포인트
- **특징**:
  - 로드밸런서(HAProxy) 연동
  - 과부하/연결실패 시 unhealthy 전환
  - 200 OK vs 503 Service Unavailable 응답
  - StatsCollector의 snapshot() 조회 (read-only)

### proxy 모듈
- **책임**: 전체 시스템 통합 및 세션 관리
- **구성**:
  - `proxy_server.hpp`: TCP 서버 (accept 루프 + graceful shutdown + SIGHUP 정책 hot reload)
  - `session.hpp`: 1:1 클라이언트-서버 릴레이 (완전 MySQL 프로토콜 파이프라인)
- **특징**:
  - **모든 모듈을 의존** (통합점)
  - Boost.Asio strand로 스레드 안전성 보장
  - Graceful Shutdown 지원 (SIGTERM/SIGINT)
  - Hot Reload 지원 (SIGHUP 시 정책 재로드)

## Query Processing Pipeline

Session::run()이 구현하는 SQL 처리 파이프라인은 다음과 같습니다:

```
1. 클라이언트 패킷 읽기 (co_await client_socket_.async_read_some)
   │
2. MySQL 프로토콜 파싱 (CommandType 판별)
   │
   ├─ COM_QUERY가 아니면 → 단계 9 (패스스루)
   │
   ▼ COM_QUERY인 경우

3. SQL 문자열 추출 (extract_command 또는 mysql_packet.payload)
   │
4. SqlParser::parse() → ParsedQuery 또는 fail-close
   │
5. InjectionDetector::check() → 인젝션 패턴 탐지 (병렬 가능)
   │
6. ProcedureDetector::detect() → 프로시저/동적 SQL 탐지 (병렬 가능)
   │
7. PolicyEngine::evaluate() → PolicyAction::kAllow/kBlock/kLog
   │
   ├─ kAllow → 단계 9 (MySQL 서버로 릴레이)
   │
   └─ kBlock/kLog → 차단 또는 로그 기록
      ├─ kBlock: Error Packet 생성 → 클라이언트 전송
      └─ kLog: MySQL 서버로 릴레이 + 감사 로그 기록

8. 모든 경로: 응답 처리 및 로깅
   ├─ StructuredLogger::log_query() (JSON 로그)
   └─ StatsCollector::on_query(blocked) (원자적 카운터)

9. MySQL 응답 릴레이 (seq_id 역전 감지)
   │
10. 루프 반복 또는 세션 종료
```

**주요 구현 포인트:**

- **ParsingError 처리**: SQL 파서가 실패하면 PolicyEngine::evaluate_error() 호출 → 반드시 kBlock 반환 (fail-close)
- **응답 릴레이**: MySQL 응답 패킷의 sequence_id가 역전되지 않도록 주의 (여러 패킷 응답 시)
- **비동기**: 모든 I/O는 `co_await` + strand으로 관리 (스레드 안전성)
- **통계 갱신**: 모든 쿼리 처리 후 StatsCollector::on_query() 호출 (blocked 여부 기록)

## Graceful Shutdown 플로우

ProxyServer가 SIGTERM/SIGINT를 수신했을 때:

```
SIGTERM/SIGINT
   │
   ▼
ProxyServer::stop() 호출
   │
   ├─ stopping_ = true (accept 루프 조기 종료)
   │
   ├─ acceptor.close() (새 연결 거부)
   │
   ├─ sessions[*].close() (진행 중인 세션 정상 종료)
   │  └─ 각 세션은 현재 쿼리 완료 후 종료
   │
   └─ session count == 0 → io_context.stop()
      └─ 모든 코루틴 종료 → main 반환
```

**특징:**
- 새 연결은 즉시 거부
- 진행 중인 쿼리는 완료 대기 (timeout 있음, 구현 시 정의)
- 정책/로그 등 external state는 정상 저장

## Hot Reload 플로우

ProxyServer가 SIGHUP을 수신했을 때:

```
SIGHUP
   │
   ▼
signal_set 핸들러
   │
   ├─ PolicyLoader::load(config_path)
   │  └─ YAML 파일 재파싱
   │
   ├─ 성공 → policy_engine_.reload(new_config)
   │  └─ std::atomic<shared_ptr> 원자적 교체
   │     ├─ 이미 진행 중인 evaluate()는 이전 config 사용
   │     └─ 새로운 evaluate() 호출은 new_config 사용
   │
   └─ 실패 → 기존 정책 유지 + 경고 로그 (fail-close 원칙)
```

**특징:**
- 무중단 정책 변경 (running 쿼리에 영향 없음)
- 로드 실패 시 기존 정책 유지 (fail-open 방지)
- 선택 사항: inotify로 정책 파일 변경 감지 시 자동 reload

## 스레드 안전성 설계

### 1. 데이터패스 (고빈도, 저레이턴시)

**원칙**: Strand를 이용한 직렬화

```cpp
// Session::run() 코루틴은 모두 strand_ 위에서 실행
co_await strand_.async_execute([this] {
    // 이 블록 내에서 concurrent 호출이 직렬화된다
    state_ = SessionState::kProcessingQuery;
    // ...
}, asio::use_awaitable);
```

**Strand의 역할:**
- Session 단위 직렬화 (다중 세션은 병렬)
- 수동 락/뮤텍스 불필요
- 코루틴과 조화로운 디자인

### 2. 통계 수집 (고빈도, 작은 연산)

**원칙**: Lock-free Atomic

```cpp
class StatsCollector {
    std::atomic<std::uint64_t> total_queries_;

    void on_query(bool blocked) noexcept {
        total_queries_.fetch_add(1, std::memory_order_relaxed);
        if (blocked) {
            blocked_queries_.fetch_add(1, std::memory_order_relaxed);
        }
    }
};
```

**특징:**
- 뮤텍스/락이 없음 (contention 제로)
- 원자성 보장 (CPU 명령어 수준)
- memory_order_relaxed로 최적화

### 3. 정책 리로드 (저빈도, 강일관성 필요)

**원칙**: Shared_ptr 원자적 교체

```cpp
class PolicyEngine {
    // 구현: std::atomic<std::shared_ptr<PolicyConfig>>

    void reload(std::shared_ptr<PolicyConfig> new_config) {
        // config_.store(new_config)
        // 이미 진행 중인 evaluate()는 이전 config로 완료됨
        // 새로운 evaluate()는 new_config로 시작
    }
};
```

**특징:**
- 진행 중인 평가와 무충돌 (새 config는 이후 요청부터 적용)
- Lock-free (C++ shared_ptr의 atomic 특성)

### 4. UDS 서버 (저빈도, read-only)

**원칙**: StatsCollector::snapshot() read-only 접근

```cpp
void UdsServer::handle_client() {
    // 쓰기 없음, 읽기만
    auto snapshot = stats_->snapshot();  // const, noexcept
    // snapshot 직렬화 → JSON → 송신
}
```

**특징:**
- 통계 수집(데이터패스)과 조회(제어패스) 완전 분리
- 뮤텍스/락 없음

## Proxy Layer 상세 다이어그램

### Session 연결 수명 관리

```
┌─────────────────────────────────────────────────────────────┐
│  ProxyServer::accept() (TCP Acceptor)                       │
│                                                             │
│  while (!stopping_) {                                       │
│    co_await acceptor.async_accept(client_socket)           │
│    co_spawn(strand, Session::run())  ← 각 세션 코루틴 생성   │
│    active_sessions++                                       │
│  }                                                          │
└──────────┬──────────────────────────────────────────────────┘
           │
    ┌──────▼──────────────────────┐
    │  Session::run()  (코루틴)     │
    │                             │
    │  ┌─ Handshake Phase         │
    │  │  HandshakeRelay          │
    │  │  client ←→ server        │
    │  │  (투명 릴레이)             │
    │  │                          │
    │  ├─ Query Processing Loop   │
    │  │  1. async_read_some()    │
    │  │  2. parse(COM_QUERY?)    │
    │  │  3. policy_engine()      │
    │  │  4. relay or block       │
    │  │  5. log + stats          │
    │  │                          │
    │  └─ Cleanup                 │
    │     server_socket.close()   │
    │     active_sessions--       │
    │                             │
    └─────────────────────────────┘
           │
    ┌──────▼──────────────────────┐
    │  MySQL Server (Port 3306)    │
    │  (Relay destination)         │
    │                             │
    └──────────────────────────────┘
```

### Query Processing 상세 흐름

```
Session::run() 루프 내부:

1. co_await client_socket_.async_read_some(buffer)
   → MySQL 패킷 읽기 (4byte seq_id + payload)

2. auto [cmd_type, sql] = extract_command(buffer)

   ├─ if (cmd_type != COM_QUERY)
   │  └─ co_await relay_packet(server_socket, buffer)
   │     (PING, INIT_DB, QUIT 등 정책 검사 없이 릴레이)
   │
   └─ if (cmd_type == COM_QUERY)
      └─ SQL 파싱 및 정책 평가 구간 시작

3. auto parsed = sql_parser_.parse(sql)
   │
   ├─ if (!parsed)  // ParseError
   │  └─ auto result = policy_engine_.evaluate_error()
   │     → 반드시 PolicyAction::kBlock 반환
   │     → Error Packet 생성 및 전송
   │     → BlockLog 기록 + stats.on_query(true)
   │
   └─ if (parsed.ok())  // ParsedQuery 획득

4. auto injection_result = injection_detector_.check(sql)
   (병렬 가능: parsed 완료 후 독립적으로 진행)
   → InjectionResult{detected, matched_pattern, ...}

5. auto procedure_result = procedure_detector_.detect(parsed)
   (병렬 가능: parsed 완료 후 독립적으로 진행)
   → ProcedureInfo{is_dynamic_sql, ...}

6. auto policy_result = policy_engine_.evaluate(
     parsed.value(),
     ctx_,
     injection_result,
     procedure_result
   )
   → PolicyResult{action=ALLOW/BLOCK/LOG, matched_rule, reason}

7. 정책 결과에 따른 분기:

   ├─ if (policy_result.action == PolicyAction::kAllow)
   │  ├─ co_await async_write(server_socket, buffer, ...)
   │  │  (MySQL 서버로 쿼리 전달)
   │  │
   │  └─ co_await relay_response(client_socket, server_socket)
   │     (서버 응답을 클라이언트로 릴레이)
   │     ├─ sequence_id 역전 감지
   │     └─ EOF 패킷 까지 모두 릴레이
   │
   ├─ if (policy_result.action == PolicyAction::kBlock)
   │  ├─ auto err_pkt = make_error_packet(
   │  │    error_code=1045,
   │  │    message="Query blocked by policy: " + reason
   │  │  )
   │  ├─ co_await async_write(client_socket, err_pkt, ...)
   │  └─ stats.on_query(blocked=true)
   │
   └─ if (policy_result.action == PolicyAction::kLog)
      ├─ co_await async_write(server_socket, buffer, ...)
      ├─ co_await relay_response(client_socket, server_socket)
      └─ 감사 로그 추가 기록

8. 모든 경로 (ALLOW/BLOCK/LOG):
   ├─ logger_.log_query(QueryLog{
   │    session_id, db_user, client_ip,
   │    raw_sql, command_type, tables,
   │    action, matched_rule, duration_us
   │  })
   │
   └─ stats_.on_query(blocked=policy_result.action==BLOCK)
      (원자적 카운터 갱신)

9. 루프 반복 또는 종료
   └─ if (COM_QUIT 또는 소켓 에러)
      └─ Session::close() → 단계 10
```

### MySQL 응답 릴레이 알고리즘 (relay_response)

```
co_await relay_response(client_socket, server_socket):

  expected_seq_id = request_seq_id + 1

  while (true) {
    auto [pkt, seq] = co_await read_mysql_packet(server_socket)

    // Sequence ID 역전 감지
    if (seq < expected_seq_id) {
      log_warning("Seq ID mismatch: expected {}, got {}",
                  expected_seq_id, seq)
      // 선택: 에러 반환 또는 경고만 기록
    }

    co_await async_write(client_socket, pkt, ...)
    expected_seq_id = seq + 1

    // EOF 패킷 감지 (ResultSet 종료)
    if (pkt.is_eof()) break
  }
```

## 배포 아키텍처

```
┌────────────────┐
│   HAProxy      │ (로드밸런싱, Health Check 모니터링)
│  Port 3306     │
└────────────────┘
        │
    ┌───┼───┬───┐
    │   │   │   │
    ▼   ▼   ▼   ▼
┌───────────┐  ┌───────────┐  ┌───────────┐
│ dbgate #1 │  │ dbgate #2 │  │ dbgate #3 │
│ :13306    │  │ :13306    │  │ :13306    │
├───────────┤  ├───────────┤  ├───────────┤
│  C++ Core │  │  C++ Core │  │  C++ Core │
│  Go CLI   │  │  Go CLI   │  │  Go CLI   │
└─────┬─────┘  └─────┬─────┘  └─────┬─────┘
      │              │              │
      └──────┬───────┴──────┬───────┘
             │              │
        ┌────▼────┐    ┌────▼────┐
        │ MySQL   │    │ MySQL   │
        │ Primary │    │ Replica │
        └─────────┘    └─────────┘

Health Check:
- dbgate #1 → GET /health:8001
- dbgate #2 → GET /health:8002
- dbgate #3 → GET /health:8003

CLI/Dashboard:
- dbgate-cli sessions  (UDS → /tmp/dbgate-1.sock)
- dbgate-cli stats
- dbgate-cli policy reload
```

## 시스템 동작 시나리오

### 정상 쿼리 통과

```
Client: SELECT * FROM users WHERE id = 1;
  │
  ├─ Parser: SELECT command, tables=[users]
  ├─ Injection: No injection pattern
  ├─ Procedure: No procedure call
  ├─ Policy: Access Rule check
  │          - user=app_service ✓
  │          - source_ip=192.168.1.x ✓
  │          - operation=SELECT ✓
  │          - table=users ✓
  │          - time_restriction: during office hours ✓
  │          → PolicyAction::kAllow
  ├─ Relay: MySQL 서버로 전송
  │         응답받아 클라이언트로 전송
  └─ Log: QueryLog (action=Allow, duration=5ms)

Server Response: ✓ 정상 응답
```

### 차단된 쿼리

```
Client: DROP TABLE users;
  │
  ├─ Parser: DROP command, tables=[users]
  ├─ Injection: No injection
  ├─ Procedure: No procedure
  ├─ Policy: SQL Rule check
  │          - block_statements includes "DROP" ✗
  │          → PolicyAction::kBlock
  ├─ Response: Error Packet
  │           (ERROR 1045: "Query blocked by policy: DROP not allowed")
  └─ Log: BlockLog (matched_rule="sql-rule-001", reason="DROP statement blocked")

Server: (요청 전달 안 됨)

Client Response: ✗ 차단 오류
```

### SQL Injection 탐지

```
Client: SELECT * FROM users WHERE name = '' OR 1=1 --';
  │
  ├─ Parser: SELECT command, tables=[users]
  ├─ Injection: Pattern ' OR 1=1 -- matched! ✗
  │           InjectionResult::detected=true
  │           matched_pattern="'\\s*OR\\s+1\\s*=\\s*1"
  ├─ Policy: Injection detected
  │         (정책에서 injection=true일 때 BLOCK 규칙 검사)
  │         → PolicyAction::kBlock
  ├─ Response: Error Packet
  │           (ERROR 1045: "Query blocked: SQL injection pattern detected")
  └─ Log: BlockLog (matched_rule="injection-detector", reason="Pattern matched: '.*OR 1=1.*'")

Client Response: ✗ 차단 오류
```

## 성능 특성

### 프록시 오버헤드

**계산 패스**:
- SQL Parser: O(1) ~ O(n) (n = SQL 길이) — 간단한 키워드 분류
- Injection Detection: O(P × n) (P = 패턴 수, n = SQL 길이)
- Policy Evaluation: O(R × M) (R = 규칙 수, M = 테이블 수)
- **총**: Microsecond 범위 (매우 빠름)

**I/O 패스**:
- MySQL 양방향 릴레이: 직접 read/write (복사 최소화)
- Asio strand: 코루틴 기반 비차단 스케줄링
- **총**: 네트워크 지연만 (프록시 추가 지연 거의 없음)

## 한계 및 미구현

### 현재 한계

1. **SQL 파서의 경량성**
   - 복잡한 서브쿼리 미지원 (테이블명 추출 불완전)
   - 주석 분할 우회: `DROP/**/TABLE` 탐지 불가
   - 인코딩 우회: URL 인코딩, hex 리터럴 미지원

2. **Injection Detection의 오탐/미탐**
   - 패턴 기반 (정규식) → false positive/negative 가능
   - ORM 생성 쿼리에서 false positive 발생 가능
   - 전처리 단계 부재 (주석 제거 미구현)

3. **Prepared Statement**
   - `COM_STMT_PREPARE` 바이너리 프로토콜 미파싱
   - 문자열 리터럴 내부 SQL 미분석

4. **세션 모델**
   - 1:1 릴레이만 지원 (커넥션 풀링 미지원)
   - Multi-statement 지원 미정

5. **DoS 방어**
   - Slowloris 같은 프로토콜 레벨 공격 미방어
   - 쿼리 실행 시간 제한 미지원

### 향후 확장

- SQL 정규화 (주석 제거, 공백 정규화)
- 풀 SQL 파서 (yacc/flex)
- eBPF로 프로세스 메타데이터 캡처
- Prometheus 메트릭 Export
- PostgreSQL 프로토콜 지원

## 구현 참고 (DON-28)

### 주요 코드 위치

**ProxyServer 및 Session 통합:**
- `src/proxy/proxy_server.hpp/cpp`: TCP 서버, accept 루프, signal 핸들링
- `src/proxy/session.hpp/cpp`: Session::run() 코루틴, 쿼리 처리 파이프라인
- `src/main.cpp`: 환경변수 설정, io_context 구성, signal_set 등록

**MySQL 프로토콜 처리:**
- `src/protocol/mysql_packet.hpp/cpp`: 패킷 파싱 및 직렬화
- `src/protocol/handshake.hpp/cpp`: 핸드셰이크 투명 릴레이
- `src/protocol/command.hpp/cpp`: CommandType 추출, COM_QUERY 여부 판별

**쿼리 처리 및 정책:**
- `src/parser/sql_parser.hpp/cpp`: SQL 파싱, ParsedQuery 생성
- `src/parser/injection_detector.hpp/cpp`: 정규식 기반 패턴 탐지
- `src/parser/procedure_detector.hpp/cpp`: 프로시저 탐지
- `src/policy/policy_engine.hpp/cpp`: 정책 평가, evaluate() 메서드
- `src/policy/policy_loader.hpp/cpp`: YAML 로더, reload() 메서드

**로깅 및 모니터링:**
- `src/logger/structured_logger.hpp/cpp`: spdlog 래퍼, log_query() 메서드
- `src/stats/stats_collector.hpp/cpp`: Atomic 통계, on_query() 메서드
- `src/stats/uds_server.hpp/cpp`: Unix Domain Socket 서버, handle_client()

**헬스 체크:**
- `src/health/health_check.hpp/cpp`: HTTP /health 엔드포인트, run() 코루틴

### 구현 팁

1. **Session::run() 구조**: 핸드셰이크 완료 후 while 루프에서 패킷 읽기 → 정책 평가 → 릴레이 반복
2. **Strand 사용**: Session 단위 모든 작업이 strand 위에서 직렬화 (멀티스레드 안전)
3. **Fail-Close**: SQL 파싱 실패 시 PolicyEngine::evaluate_error() 호출 필수
4. **Signal Handling**: ProxyServer가 signal_set 등록, graceful shutdown 구현
5. **Hot Reload**: SIGHUP 수신 시 PolicyLoader::load() → PolicyEngine::reload() 호출

## 통합 구현 요약 (DON-28)

이 섹션은 Session::run(), ProxyServer::run() 등 핵심 코루틴의 구조와 설계 원칙을 정리합니다.

### Session::run() 코루틴의 역할

Session 인스턴스 하나는 클라이언트 연결 하나에 해당하며, Session::run() 코루틴이 다음을 수행합니다:

1. **핸드셰이크 (HandshakeRelay)**: 클라이언트 ↔ 서버 간 MySQL 핸드셰이크 패킷을 투명 릴레이
2. **Query Processing Loop**:
   - 클라이언트로부터 MySQL 패킷 읽기 (co_await async_read_some)
   - CommandType 판별 (COM_QUERY vs 기타)
   - COM_QUERY인 경우 SQL 파싱 및 정책 평가
   - 정책 결과에 따라 릴레이/차단
   - 응답을 클라이언트로 릴레이
   - 로깅 및 통계 갱신
3. **정리 (Cleanup)**: 세션 종료 시 소켓 정상 종료

### ProxyServer의 역할

ProxyServer 인스턴스는 다음을 관리합니다:

1. **TCP Accept 루프**: 클라이언트 연결을 수용하고 각 연결마다 Session 코루틴 생성
2. **Signal Handling**: SIGTERM/SIGINT → graceful shutdown, SIGHUP → hot reload
3. **Session 관리**: active session count, graceful shutdown 시 모든 세션 종료 대기

### Fail-Close 원칙 구현

- **SQL 파싱 실패**: PolicyEngine::evaluate_error() 호출 → 반드시 PolicyAction::kBlock 반환
- **정책 엔진 오류**: 설정 로드 실패 등 → 기본값 kBlock
- **무매칭**: 명시적 allow 규칙이 없으면 default deny (kBlock)

### Strand를 이용한 스레드 안전성

- Session 단위 코루틴이 모두 동일 strand 위에서 실행
- 수동 락/뮤텍스 불필요
- 다중 세션은 병렬 실행 (각각 다른 strand 사용 가능)

### Lock-Free 통계 수집

- StatsCollector::on_query()는 std::atomic만 사용 (contention 제로)
- HealthCheck::run(), UdsServer::run()은 snapshot()을 호출하여 현재 통계 조회 (read-only)

### Hot Reload 메커니즘

```
SIGHUP 신호
  → signal_set 핸들러
    → PolicyLoader::load(config_path)
      → 성공: PolicyEngine::reload(new_config)
      → 실패: 로그 기록, 기존 정책 유지 (fail-close)
```

- std::atomic<shared_ptr<PolicyConfig>>로 원자적 교체
- 진행 중인 evaluate()는 영향 없음 (이전 config 사용)
- 새로운 요청부터 새 정책 적용

### Graceful Shutdown 시나리오

```
SIGTERM/SIGINT 신호
  → signal_set 핸들러
    → ProxyServer::stop()
      → stopping_ = true
      → acceptor.close() (새 연결 거부)
      → sessions[*].close() (기존 세션 정상 종료 대기)
      → io_context.stop()
```

## 참고 문서

- ADR-001: Boost.Asio vs raw epoll
- ADR-002: Handshake Passthrough 설계
- ADR-004: YAML 정책 형식
- ADR-005: C++/Go 언어 분리
- ADR-006: SQL 파서 범위
- `docs/data-flow.md`: 시나리오별 상세 흐름
- `docs/uds-protocol.md`: Go CLI ↔ C++ 통신 프로토콜
