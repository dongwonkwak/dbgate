# dbgate 데이터 흐름 문서

## 목적
- 클라이언트 요청이 `dbgate`를 거쳐 MySQL 서버로 전달되고 응답이 돌아오는 흐름을 설명한다.
- 정책 판정, 차단, 에러 처리, 관측성 포인트가 어디서 발생하는지 명확히 한다.

## 적용 범위
- 데이터패스(C++): `src/protocol/`, `src/proxy/`, `src/parser/`, `src/policy/`, `src/logger/`, `src/stats/`, `src/health/`
- 컨트롤플레인(Go)은 본 문서에서 직접 다루지 않으며, UDS 연동은 `docs/uds-protocol.md` 참고

## 관련 문서
- `docs/architecture.md`
- `docs/interface-reference.md`
- `docs/uds-protocol.md`
- `docs/policy-engine.md` (작성 예정/존재 시)

## 공통 전제
- 핸드셰이크는 패스스루(프록시가 인증에 개입하지 않음)
- 정책 엔진 오류 시 `fail-close`
- SQL 파서는 경량 파서(키워드 + 정규식 기반)

## 흐름 분류
본 문서는 최소 아래 흐름을 유지한다.

1. 정상 쿼리 통과 흐름 (`ALLOW`)
2. 정책 차단 흐름 (`BLOCK`)
3. 파싱 실패 / 정책 오류 흐름 (`fail-close`)
4. 비-`COM_QUERY` 커맨드 패스스루 흐름
5. 세션 종료/에러 종료 흐름
6. Health Check 조회 흐름
7. 정책 Hot Reload 흐름
8. Graceful Shutdown 흐름

## 데이터 흐름 템플릿 (시나리오별로 반복)

### 시나리오 이름
- 입력:
- 사전 조건:
- 주요 컴포넌트:
- 출력/결과:

#### 단계
1. 
2. 
3. 

#### 관측성 포인트
- 로그:
- 통계:
- 헬스체크 영향:

#### 문서화해야 하는 한계/주의점
- 

## 시나리오 1: 정상 쿼리 통과 흐름 (초안)

### 입력
- 클라이언트가 `COM_QUERY` 패킷으로 SQL 전송

### 사전 조건
- 세션 핸드셰이크 완료
- 정책 로드 성공 상태

### 주요 컴포넌트
- `proxy/session`
- `protocol/command`
- `parser/*`
- `policy/policy_engine`
- `logger`, `stats`

### 출력/결과
- 쿼리 릴레이 수행
- 서버 응답 전달
- 로그/통계 갱신

#### 단계
1. 세션이 클라이언트 패킷을 읽고 `COM_QUERY` 여부를 판별한다.
2. SQL 문자열을 추출하고 파서/탐지/정책 엔진에 전달한다.
3. 정책 판정이 `ALLOW`이면 업스트림 MySQL로 릴레이한다.
4. 응답을 클라이언트로 전달하고 로그/통계를 갱신한다.

#### 관측성 포인트
- 로그: 세션/쿼리/판정 결과
- 통계: 총 쿼리 수, 차단 수(해당 없음), 활성 세션

#### 문서화해야 하는 한계/주의점
- SQL 파싱 범위 밖 쿼리는 보수적으로 처리되는지 확인 필요

## 시나리오 2: 정책 차단 흐름 (BLOCK)

### 입력
- 클라이언트가 `COM_QUERY` 패킷으로 차단 대상 SQL 전송 (예: `DROP TABLE users`)

### 사전 조건
- 세션 핸드셰이크 완료
- 정책 로드 성공 상태

### 주요 컴포넌트
- `proxy/session`, `protocol/command`, `parser/sql_parser`, `policy/policy_engine`, `logger`, `stats`

### 출력/결과
- MySQL 서버로 릴레이하지 않음
- 클라이언트에 MySQL ERR 패킷 반환
- 차단 로그(`BlockLog`) 기록, 통계 갱신

#### 단계
1. 세션이 `COM_QUERY` 패킷을 수신하고 SQL 문자열을 추출한다.
2. `SqlParser::parse()`로 구문 분류(`SqlCommand::kDrop` 등) 및 테이블명 추출.
3. `InjectionDetector::check()`로 인젝션 패턴 검사 (UNION/OR/SLEEP 등 10개 기본 패턴).
4. `ProcedureDetector::detect()`로 CALL/PREPARE/EXECUTE 여부 확인.
5. `PolicyEngine::evaluate()`가 `kBlock` 반환.
6. 세션이 MySQL ERR 패킷을 생성하여 클라이언트로 전송한다.
7. `log_block()` 호출, `stats.on_query(blocked=true)`.

#### 관측성 포인트
- 로그: `BlockLog` (session_id, db_user, client_ip, raw_sql, matched_rule, reason)
- 통계: `total_queries++`, `blocked_queries++`, block_rate 갱신

#### 문서화해야 하는 한계/주의점
- `InjectionDetector` 패턴은 config에서 주입; 빈 패턴 목록은 fail-close(전체 차단)
- `SqlParser` 파싱 실패 시 시나리오 3으로 전환

---

## 시나리오 3: 파싱 실패 / 정책 오류 (fail-close)

### 입력
- 파싱 불가 SQL 또는 정책 엔진 오류

### 사전 조건
- 세션 핸드셰이크 완료

### 주요 컴포넌트
- `parser/sql_parser`, `policy/policy_engine`, `logger`, `stats`

### 출력/결과
- 무조건 차단 (fail-close 원칙)
- 클라이언트에 ERR 패킷 반환

#### 단계
1. `SqlParser::parse()` 실패 → `std::unexpected(ParseError)` 반환.
2. 세션이 `PolicyEngine::evaluate_error()`를 호출 → 반드시 `kBlock` 반환.
3. ERR 패킷 전송, `log_block()`, `stats.on_query(blocked=true)`.

#### 관측성 포인트
- 로그: `BlockLog` (reason에 파서 오류 메시지 포함)
- 통계: `blocked_queries++`

#### 문서화해야 하는 한계/주의점
- fail-open 동작은 아키텍처 원칙상 금지
- `InjectionDetector` 유효 패턴 없음 → 생성자에서 `fail_close_active_=true` 설정, `check()` 전체 차단
- [DON-39] trailing 세미콜론 처리: `SELECT 1;` 처럼 세미콜론 뒤에 공백/개행만 있는 경우는
  단일 구문으로 허용(parse 성공). 멀티 스테이트먼트(`SELECT 1; DROP TABLE users` 등)는
  세미콜론 뒤에 non-whitespace 문자가 있으므로 여전히 파싱 실패 → fail-close 경로로 처리.

---

## 시나리오 4: COM_QUERY 외 커맨드 패스스루

### 입력
- `COM_QUIT`, `COM_PING`, `COM_INIT_DB` 등 비-쿼리 커맨드

### 출력/결과
- 정책 검사 없이 MySQL 서버로 투명 릴레이

#### 단계
1. `extract_command()`가 `CommandType::kComQuery` 이외의 타입 반환.
2. 세션이 파서/정책 단계를 건너뛰고 패킷을 업스트림으로 전달.
3. 서버 응답을 클라이언트로 전달.

#### 관측성 포인트
- 로그: 커맨드 타입에 따라 선택적 기록
- 통계: `total_queries` 미증가 (쿼리 아님)

---

## 시나리오 5: 세션 종료 / 에러 종료

### 입력
- 클라이언트 `COM_QUIT`, 소켓 EOF, 타임아웃

### 출력/결과
- 세션 `kClosed` 상태 전환
- 서버 측 소켓 정리

#### 단계
1. `COM_QUIT` 수신 또는 읽기 오류 감지.
2. 업스트림 소켓 닫기.
3. `log_connection(event="disconnect")`, `stats.on_connection_close()`.
4. 세션 객체 소멸 (`shared_ptr` 참조 해제).

---

## 시나리오 6: Health Check 조회 흐름

### 입력
- 로드밸런서(HAProxy) HTTP GET /health:8080

### 사전 조건
- HealthCheck::run() 실행 중
- TCP 포트 8080 listen 중

### 주요 컴포넌트
- `health/health_check`, `stats/stats_collector`

### 출력/결과
- HTTP 응답: 200 OK + JSON (또는 503 Service Unavailable)

#### 단계
1. HealthCheck가 HTTP GET /health 요청 수신
2. status_ 확인:
   - `kHealthy` → 200 OK 응답
   - `kUnhealthy` → 503 Service Unavailable 응답
3. JSON 바디 (선택):
   ```json
   {
     "status": "healthy",
     "active_sessions": 42,
     "total_queries": 123456,
     "blocked_queries": 78,
     "block_rate": "0.063%"
   }
   ```
4. stats_.snapshot() 호출로 현재 통계 획득 (read-only, lock-free)

#### 관측성 포인트
- 로그: 헬스체크 상태 변경만 기록 (verbose 로깅 아님)
- 통계: 헬스체크 자체는 통계에 포함 안 함 (메타 데이터)

#### 문서화해야 하는 한계/주의점
- 헬스체크는 최소 요청 처리 능력만 테스트 (과부하 감지는 application layer)
- HTTP/1.0만 지원 (HAProxy 최소 호환성)

---

## 시나리오 7: 정책 Hot Reload 흐름

### 입력
- 프로세스가 SIGHUP 신호 수신 또는 정책 파일 변경 감지

### 사전 조건
- ProxyServer::run() 실행 중
- signal_set이 SIGHUP 핸들러 등록

### 주요 컴포넌트
- `proxy/proxy_server`, `policy/policy_loader`, `policy/policy_engine`

### 출력/결과
- 새 정책 로드 성공 → 기존 쿼리는 이전 정책, 새 쿼리는 새 정책으로 평가
- 로드 실패 → 기존 정책 유지, 경고 로그 기록 (fail-close)

#### 단계
1. SIGHUP 신호 수신 → signal_set 핸들러 트리거
2. PolicyLoader::load(config_path) 호출
3. YAML 파싱 성공:
   - PolicyEngine::reload(new_config) 호출
   - std::atomic<shared_ptr<PolicyConfig>>로 원자적 교체
   - 기존 evaluate() 진행 중이어도 이전 config 사용 (thread-safe)
4. 파싱 실패:
   - 로그: "Failed to reload policy: {error}" (WARN/ERROR)
   - 기존 정책 유지, 서비스 중단 없음

#### 관측성 포인트
- 로그: PolicyLoader 성공/실패 결과 기록
- 통계: reload 횟수 추적 (선택)
- 모니터링: "policy_reload_failures" 메트릭

#### 문서화해야 하는 한계/주의점
- 이미 평가 중인 쿼리는 재평가되지 않음 (새 요청부터 적용)
- YAML 문법 오류는 전체 로드 실패 처리 (부분 업데이트 없음)

---

## 시나리오 8: Graceful Shutdown 흐름

### 입력
- 프로세스가 SIGTERM 또는 SIGINT 신호 수신

### 사전 조건
- ProxyServer::run() 실행 중
- 활성 세션 0개 이상

### 주요 컴포넌트
- `proxy/proxy_server`, `proxy/session`

### 출력/결과
- 모든 활성 세션 정상 종료
- 새 연결 거부
- 프로세스 종료

#### 단계
1. SIGTERM/SIGINT 수신 → signal_set 핸들러 트리거
2. ProxyServer::stop() 호출
3. stopping_ = true 설정 (accept 루프 조기 종료)
4. tcp::acceptor.close() (포트 해제, 새 연결 즉시 거부)
5. sessions[*].close() 호출:
   - 각 세션의 client_socket, server_socket 정상 종료
   - 진행 중인 쿼리 처리 완료 대기
   - timeout 적용 (선택, 구현 시 정의)
6. 모든 세션 종료 대기 (co_await)
7. io_context.stop() 호출
8. main() 반환 → 프로세스 종료

#### 관측성 포인트
- 로그: "Shutting down gracefully", 최종 활성 세션 수 기록
- 통계: 총 처리한 쿼리 수, 세션 수 저장 (다음 시작 시 참고)
- 타이밍: shutdown이 몇 초 걸렸는지 기록

#### 문서화해야 하는 한계/주의점
- timeout 없이 무한 대기 가능 (구현에서 timeout 설정 권장)
- 진행 중인 트랜잭션은 롤백되지 않음 (MySQL 커넥션 종료 시 자동 롤백)
- 정책/로그 파일 손상 방지: 마지막 쓰기 flush 필요

## 코드 참고 경로 (DON-28)

각 시나리오를 구현할 때 다음 파일들을 참조하세요:

| 시나리오 | 주요 코드 경로 |
|---------|---------------|
| 정상 쿼리 통과 (시나리오 1) | `src/proxy/session.cpp` (run()), `src/policy/policy_engine.cpp` (evaluate()) |
| 정책 차단 (시나리오 2) | `src/proxy/session.cpp` (create_error_packet()), `src/logger/structured_logger.cpp` (log_block()) |
| 파싱 실패 (시나리오 3) | `src/parser/sql_parser.cpp` (parse()), `src/policy/policy_engine.cpp` (evaluate_error()) |
| COM_QUERY 외 패스스루 (시나리오 4) | `src/protocol/command.cpp` (extract_command()), `src/proxy/session.cpp` (relay 로직) |
| 세션 종료 (시나리오 5) | `src/proxy/session.cpp` (close()), `src/logger/structured_logger.cpp` (log_connection()) |
| Health Check (시나리오 6) | `src/health/health_check.cpp` (run()), `src/stats/stats_collector.cpp` (snapshot()) |
| Hot Reload (시나리오 7) | `src/proxy/proxy_server.cpp` (signal 핸들러), `src/policy/policy_loader.cpp` (load()) |
| Graceful Shutdown (시나리오 8) | `src/proxy/proxy_server.cpp` (stop()), `src/main.cpp` (signal_set) |

## 모듈 구현 상태 (DON-28 기준)

이 문서는 다음 모듈의 완전 구현을 기준으로 작성되었습니다:

- `src/protocol/`: MySQL 패킷 파싱, 핸드셰이크 패스스루, COM_QUERY 추출 ✓
- `src/parser/`: SQL 파싱, Injection 탐지, 프로시저 탐지 ✓
- `src/policy/`: 정책 엔진, YAML 로더, Hot Reload ✓
- `src/logger/`: 구조화 JSON 로깅 ✓
- `src/stats/`: Atomic 통계, UDS 서버 ✓
- `src/health/`: HTTP /health 엔드포인트 ✓
- `src/proxy/`: ProxyServer (accept 루프), Session (Query Processing Pipeline) ✓
- `src/main.cpp`: signal handling (SIGTERM, SIGHUP), graceful shutdown, hot reload ✓

각 시나리오는 실제 구현된 코드 경로를 기준으로 작성되었습니다. 구현 변경 시 이 문서도 함께 업데이트해야 합니다.

## 변경 체크리스트 (문서 유지보수용)
- 데이터 흐름 단계가 실제 구현과 일치하는가?
- 파서/정책/로깅/통계 호출 순서가 바뀌었는가?
- 에러 경로/차단 경로가 누락되지 않았는가?
- 관련 테스트/README/운영 문서와 충돌하지 않는가?
- graceful shutdown / hot reload 흐름이 구현과 일치하는가?
- UDS 서버 및 Health Check HTTP 엔드포인트가 정확히 문서화되었는가?
