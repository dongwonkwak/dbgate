# ADR-002: MySQL 핸드셰이크 패스스루

## Status

Accepted

## Context

MySQL 클라이언트와 서버가 연결을 초기화할 때, 핸드셰이크(Handshake)라는 프로토콜 단계를 거친다. 이 단계에서 서버는 클라이언트에게 지원하는 인증 방식(authentication plugin)과 프로토콜 버전, 서버 역량(capability flags)을 전달하고, 클라이언트는 사용자 인증을 수행한 후 연결을 확립한다.

dbgate는 MySQL 클라이언트와 서버 사이의 투명 프록시로, 핸드셰이크를 처리하는 방식에 두 가지 선택지가 있다:

### 선택지 1: 프록시가 직접 핸드셰이크 수행
프록시가 클라이언트와 서버 각각과 독립적으로 핸드셰이크를 수행한다. 클라이언트는 프록시와 인증하고, 프록시가 별도로 서버와 인증한다. 이 경우 프록시는 다음을 구현해야 한다:

- 서버로부터 Handshake Init 패킷 수신 → 클라이언트에게 relay
- 클라이언트의 Handshake Response 수신 → 모든 auth plugin 구현 필요
- 인증 성공/실패 판정 후 서버로 접속

**장점:**
- 프록시가 사용자 인증 정보를 명확히 파악 → 감사(audit) 용이
- 프록시 수준에서 특정 사용자 거부 가능

**단점:**
- MySQL의 모든 auth plugin 구현 필요: caching_sha2_password, ed25519, LDAP, PAM 등
- 새로운 인증 방식 추가 시 프록시도 코드 변경 필수
- 버전별 프로토콜 변경 대응 부담 높음
- 복잡한 인증 로직으로 인한 보안 버그 위험

### 선택지 2: 핸드셰이크 패스스루 (투명 릴레이)
프록시는 핸드셰이크 패킷을 해석하지 않고 투명하게 클라이언트 ↔ 서버 간 릴레이한다. 클라이언트와 서버가 직접 인증을 수행하고, 핸드셰이크 완료 후 프록시는 COM_QUERY 단계부터 SQL 정책을 적용한다.

**장점:**
- 모든 auth plugin 자동 지원 → MySQL 버전 업그레이드 시 대응 불필요
- 프록시 코드 단순화 → 버그 가능성 감소
- 클라이언트-서버 인증이 변경되어도 프록시에 영향 없음

**단점:**
- 사용자 정보 추출을 위해 핸드셰이크 응답 패킷을 최소 파싱해야 함
- 서버가 인증을 거부한 경우(Invalid Password 등)를 감지하려면 에러 패킷 해석 필요
- 인증 실패를 로그에 기록하기 위해서는 핸드셰이크 에러 응답 패싱 필수

## Decision

**핸드셰이크 패스스루**를 채택한다.

프록시는 핸드셰이크 패킷을 투명하게 릴레이하며, 핸드셰이크 완료 후 COM_QUERY 단계부터 SQL 정책을 적용한다.

SessionContext에 db_user와 db_name 필드를 채우기 위해 다음을 수행한다:
- 클라이언트의 Handshake Response 패킷에서 username, database 필드를 최소 파싱
- 서버의 Handshake Init 패킷(sequence_id=0)과 OK/Error 응답(sequence_id=2)을 인식하여 핸드셰이크 완료 판단
- 에러 응답 수신 시 "Authentication Failed" 로그 기록

## Consequences

### Positive

- **Auth Plugin 호환성**: MySQL 5.7의 mysql_native_password부터 MySQL 8.0+의 caching_sha2_password, Ed25519 등 모든 플러그인 자동 지원. 새로운 인증 방식이 추가되어도 프록시 수정 불필요.

- **구현 단순성**: 독립적인 인증 로직 구현 불필요. 핸드셰이크 초기/응답 패킷의 기본 필드만 파싱하면 되므로 코드 복잡도 낮음.

- **MySQL 업그레이드 대응**: 버전 업그레이드 시 새로운 capability flags, 플러그인이 추가되어도 프록시는 투명 릴레이만 수행하므로 유지보수 부담 제로.

### Negative

- **사용자 정보 추출 비용**: SessionContext 채우기 위해 Handshake Response 패킷의 username, database 필드를 파싱해야 함. 클라이언트의 인증 정보가 프록시를 거쳐야만 추출 가능.

- **감사 추적성 제한**: 서버가 인증을 거부한 경우(Invalid Password, Access Denied 등) 프록시는 에러 OK 패킷을 감지해야만 로그 기록 가능. 프록시 수준에서 사용자 거부/강제 차단 불가능.

- **프로토콜 변경 추적 필요**: MySQL 바이너리 프로토콜이 변경될 경우 최소 파싱 로직 검증 필요. 다만 이는 선택지 1 대비 훨씬 경미한 수준.

## Alternatives Considered

### 선택지 1: 프록시 직접 핸드셰이크 구현

프록시가 모든 MySQL auth plugin을 직접 구현하는 방식.

**배제 이유:**
- caching_sha2_password, Ed25519, LDAP, PAM 등 수십 가지 플러그인 구현 필수
- MySQL 5.7, 8.0, 8.4 등 버전별 프로토콜 차이 대응 필수
- 각 플러그인별 테스트/보안 감시 필요 → 개발/유지보수 비용 지수 증가
- 보안 버그(timing attack, side-channel) 위험 높음
- 핵심 비즈니스 로직(SQL 파싱, 정책 엔진)에 집중하기 위해 부적합

### 선택지 3: 핸드셰이크 캐싱

핸드셰이크 정보를 세션별로 캐시하여 재사용하는 방식.

**배제 이유:**
- MySQL 인증은 매번 새로운 nonce 기반 challenge-response 수행
- 캐시 재사용은 보안 위협(nonce 예측, replay attack)
- 세션마다 고유한 인증이 필요하므로 실질적으로 불가능

## Related ADRs

- ADR-001: Boost.Asio 사용으로 양방향 패킷 릴레이 구현
- ADR-005: C++ 데이터패스에서 프로토콜 처리, Go에서 정책 관리 분리

## Implementation Notes

- Handshake 완료 판단: 클라이언트/서버 모두 OK 패킷 교환 후
- Username/Database 추출: Handshake Response 패킷의 offset 고정 위치에서 파싱
- 에러 처리: ERR_Packet(type=0xFF) 감지 → 인증 실패 로그 기록
- 멀티스레드 안전: Boost.Asio strand 내에서 Handshake 완료 판단 → data race 불가능
