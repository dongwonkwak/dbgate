# dbgate 데이터 흐름 문서

## 목적
- 클라이언트 요청이 `dbgate`를 거쳐 MySQL 서버로 전달되고 응답이 돌아오는 흐름을 설명한다.
- 정책 판정, 차단, 에러 처리, 관측성 포인트가 어디서 발생하는지 명확히 한다.

## 적용 범위
- 데이터패스(C++): `src/protocol/`, `src/proxy/`, `src/parser/`, `src/policy/`, `src/logger/`, `src/stats/`
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

## 변경 체크리스트 (문서 유지보수용)
- 데이터 흐름 단계가 실제 구현과 일치하는가?
- 파서/정책/로깅/통계 호출 순서가 바뀌었는가?
- 에러 경로/차단 경로가 누락되지 않았는가?
- 관련 테스트/README/운영 문서와 충돌하지 않는가?
