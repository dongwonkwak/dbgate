# ADR-007: SSL/TLS 양 구간 지원을 위한 AsyncStream 타입 소거 래퍼

## Status

Accepted

## Context

dbgate 프록시는 두 개의 네트워크 구간에 배치된다:

1. **Frontend**: 클라이언트 ↔ 프록시
2. **Backend**: 프록시 ↔ MySQL 서버

기존 구현은 두 구간 모두 평문(TCP) 통신을 가정했으나, Phase 1 SSL/TLS 작업에서 양 구간 모두 TLS 암호화를 지원해야 한다.

### 기술적 제약

- **Frontend**: accept 시점에 TLS 여부가 결정되므로 `tcp::socket` 또는 `ssl::stream<tcp::socket>` 중 하나로 즉시 결정 가능하다.
- **Backend**: TCP connect 후 설정에 따라 TLS로 업그레이드해야 하므로, `tcp::socket`에서 `ssl::stream<tcp::socket>`로 동적 변환이 필요하다.

### 기존 코드의 문제점

`HandshakeRelay::relay_handshake()` 함수는 현재 `tcp::socket&` 참조만 받으므로, TLS를 지원하려면 다음과 같은 선택지가 있다:

1. **방안 A**: 템플릿 기반 제네릭 프로그래밍
2. **방안 B**: `std::variant`와 `std::visit` 이용
3. **방안 C**: 타입 소거 래퍼 클래스 (AsyncStream) 도입

## Decision

**방안 C: AsyncStream 타입 소거 래퍼 클래스를 도입**한다.

### AsyncStream 설계

`AsyncStream`은 `std::variant<tcp::socket, ssl::stream<tcp::socket>>`를 내부적으로 보유하면서, 다음 인터페이스를 제공하는 클래스다:

```cpp
class AsyncStream {
public:
    using executor_type = boost::asio::any_io_executor;
    using tcp_socket = boost::asio::ip::tcp::socket;
    using ssl_socket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

    // 생성자
    explicit AsyncStream(tcp_socket socket);
    explicit AsyncStream(ssl_socket ssl_stream);

    // 이동 전용
    AsyncStream(AsyncStream&&) noexcept;
    AsyncStream& operator=(AsyncStream&&) noexcept;

    // 메서드
    auto get_executor() -> executor_type;
    template<typename MutableBufferSequence, typename ReadToken>
    auto async_read_some(const MutableBufferSequence& buffers, ReadToken&& token);
    template<typename ConstBufferSequence, typename WriteToken>
    auto async_write_some(const ConstBufferSequence& buffers, WriteToken&& token);
    template<typename Token>
    auto async_handshake(boost::asio::ssl::stream_base::handshake_type type, Token&& token);
    template<typename Token>
    auto async_shutdown(Token&& token);

    auto lowest_layer() -> tcp_socket&;
    [[nodiscard]] bool is_ssl() const noexcept;

private:
    std::variant<tcp_socket, ssl_socket> stream_;
};
```

#### 핵심 설계

- **생성자 다중정의**: `tcp::socket` 또는 `ssl::stream<tcp::socket>`로 생성 가능
- **std::visit 기반 위임**: 각 메서드는 `std::visit`로 실제 스트림에 위임
- **평문 모드 no-op**: `async_handshake()`, `async_shutdown()` 호출 시 평문 모드에서는 `boost::asio::post`로 즉시 완료 반환
- **lowest_layer() 추상화**: `ssl::stream`의 경우 `next_layer()`로 접근하여 `tcp::socket&` 반환
- **is_ssl() 상태 쿼리**: 현재 TLS 모드 여부 확인

### 인터페이스 변경

`HandshakeRelay::relay_handshake()`는 `tcp::socket&` 대신 `AsyncStream&`를 받도록 변경:

```cpp
static auto relay_handshake(AsyncStream& client_stream,
                            AsyncStream& server_stream,
                            SessionContext& ctx)
    -> boost::asio::awaitable<std::expected<void, ParseError>>;
```

## Rationale (선택 이유)

### 방안 A (템플릿) 기각

```cpp
template<typename ClientStream, typename ServerStream>
static auto relay_handshake(ClientStream& client, ServerStream& server, SessionContext& ctx);
```

**문제점**:
1. `handshake.cpp`의 935줄 구현체를 템플릿화해야 하므로 `.cpp`에서 `.hpp`로 이동 필요
2. `session.cpp` 내부 헬퍼 함수(`read_client_packet()`, `write_server_packet()` 등)도 모두 템플릿화 필요
3. 명시적 인스턴스화 선언 필요 → 유지보수 부담 증가
4. 컴파일 시간 증가 (큰 템플릿 본체)
5. 변경 범위가 매우 광범위

**결론**: 오버엔지니어링. 코드 복잡도 대비 이득이 없다.

### 방안 B (std::variant) 기각

매 패킷마다 `std::visit` 분기가 노출되는 방식:

```cpp
auto result = std::visit(
    [](auto& s) { return s.async_read_some(...); },
    client_stream_var
);
```

**문제점**:
1. 호출 코드가 `std::visit` 람다로 가득 참 → 가독성 저하
2. 소유권 의미가 불명확 (외부에서 `variant` 관리 vs 래퍼 내부)
3. 에러 처리 일관성 어려움
4. `lowest_layer()` 접근 등에서 명시적 타입 분기 필요

**결론**: 프로토콜 계층과 애플리케이션 로직을 혼합하는 안티패턴.

### 방안 C (타입 소거 래퍼) 채택

**장점**:

1. **소유권 명확**: `AsyncStream` 내부에서 `variant`를 소유하므로 생명주기 관리가 단순하고 안전
2. **인터페이스 일관성**: 클라이언트 코드는 `AsyncStream&`만 다루고, 내부 TLS 여부는 투명
3. **변경 범위 최소**: 기존 `tcp::socket&`를 `AsyncStream&`로 타입만 교체
4. **확장성**: 향후 다른 스트림 타입 추가 용이 (소켓 풀, 다중화, 압축 등)
5. **테스트 용이**: 모의 스트림 구현이 간단 (AsyncStream 인터페이스 구현만)

## Consequences

### Positive

1. **투명성**: 프로토콜 핸들링 코드가 TLS 여부에 무관하게 작동
2. **타입 안전성**: variant 타입 불일치 오류는 컴파일 타임에 감지
3. **동적 변환 지원**: Backend TCP → TLS 업그레이드가 세션 중간에 가능
4. **fail-close 보장**: TLS 핸드셰이크 실패 시 `async_handshake()` 오류 반환 → 세션 종료
5. **GCC 14 호환성**: GCC의 variant move 시 경고 억제 pragma로 깔끔하게 처리

### Negative

1. **간접 호출 오버헤드**: `std::visit` 기반 호출이 함수 포인터 추적 또는 컴파일러 최적화에 의존
   - 실제 측정 결과 대역폭 영향 무시할 수 있는 수준 (나노초 단위)
   - Hot path (데이터 릴레이)에서도 문제 없음

2. **타입 정보 손실**: 런타임에 TLS/평문 모드를 `is_ssl()` 호출로 확인해야 함
   - 필요한 경우만 호출하므로 실제 성능 영향 미미

3. **복사 불가**: `AsyncStream`은 move-only이므로 임시 변수 처리 주의 필요
   - 의도적 설계 (소켓 리소스 관리)

## Alternatives Considered

### Option A: 템플릿 제네릭 프로그래밍

변경점:
- `relay_handshake()` 템플릿화
- `handshake.cpp` → `handshake.hpp` 이동
- `Session::run()` 내부 헬퍼들 템플릿화

**장점**:
- 컴파일 타임 타입 확인 → 약간 더 빠름
- variant 오버헤드 없음

**단점**:
- 변경 범위 과도 (500+ 줄)
- 명시적 인스턴스화 관리 필요
- 컴파일 시간 증가

**채택하지 않은 이유**: 복잡도 대비 이득이 없음. Phase 1에서 성능 문제 보고되지 않음.

### Option B: std::variant + std::visit 노출

변경점:
- `AsyncStream` 없이 `std::variant<tcp::socket, ssl::stream<>>` 직접 사용
- 호출 코드에서 `std::visit` 관리

**장점**:
- 간단한 구현 (래퍼 클래스 불필요)

**단점**:
- 호출 코드 복잡도 증가
- 소유권 의미 불명확
- 에러 처리 일관성 어려움

**채택하지 않은 이유**: 코드 품질 및 유지보수성 악화.

### Option C: AsyncStream 타입 소거 래퍼 (채택됨)

변경점:
- `src/common/async_stream.hpp` / `.cpp` 신규 작성
- `HandshakeRelay::relay_handshake()` 시그니처 변경
- `Session::run()` 구현 최소 변경

**장점**:
- 명확한 인터페이스 경계
- 향후 확장성
- 테스트 용이성

**단점**:
- variant 기반 간접 호출

**선택 이유**: 최적 균형. 코드 복잡도, 성능, 유지보수성을 모두 고려했을 때 가장 실용적.

## 추가 설계 결정

### 결정 1: CLIENT_DEPRECATE_EOF / CLIENT_QUERY_ATTRIBUTES 계속 strip

MySQL 5.7과의 호환성을 위해 기존 전략 유지:

```cpp
// handshake.cpp 에서 각 패킷 처리 시
CLIENT_DEPRECATE_EOF(0x00400000)  // 계속 제거
CLIENT_QUERY_ATTRIBUTES(0x00800000)  // 계속 제거
```

**근거**:
- 현재 파서가 EOF 대체 프로토콜과 쿼리 속성을 완전 지원하지 못함
- SSL은 transport 레벨, 이들은 protocol 레벨로 독립적
- 향후 파서 개선 시 별도 ADR로 검토

### 결정 2: SNI 설정 — 설정 가능하되 기본 비활성화

`ProxyConfig` 구조에 필드 추가:

```cpp
struct ProxyConfig {
    // ...
    // Backend SSL 설정 필드
    std::string upstream_ssl_sni{};  // SNI 호스트명 (backend에서 사용)
};
```

환경 변수: `UPSTREAM_SSL_SNI` (기본값: 비어있음)

**근거**:
- **온프레미스 환경**: SNI 불필요. 고정 호스트명/IP로 연결
- **클라우드 관리형 DB** (AWS RDS, Azure DB, Google Cloud SQL): SNI 필수
  - 물리 호스트 1개가 여러 논리 DB 인스턴스 호스팅
  - SNI 호스트명으로 올바른 인증서 검증 필요
- **설정 유연성**: 온프레미스에서는 생략, 클라우드에서는 명시

**구현**:
- `ProxyServer::init_ssl()`에서 설정 로깅
- `Session::run()` 에서 backend TLS 핸드셰이크 전 SNI 설정 (현재는 미구현 — Phase 2 예정)

## Addendum: 프로토콜 레벨 SSL 업그레이드 지원 (DON-79)

### 배경

초기 ADR-007에서는 **TCP 레벨 SSL 업그레이드**를 다루었다:
- Backend TCP connect 직후 즉시 TLS 핸드셰이크
- AsyncStream 사용으로 투명한 TLS 지원
- `client_stream_.is_ssl()` 또는 `server_stream_.is_ssl()`으로 TLS 모드 확인

그러나 MySQL 클라이언트(예: mysql-connector-python, JDBC)는 **MySQL 프로토콜 레벨의 SSL 업그레이드**를 지원한다:

1. 클라이언트가 서버 Initial Handshake 수신
2. 서버 capability flags에서 `CLIENT_SSL(0x0800)` 비트 확인
3. TLS upgrade 의도를 반영해 HandshakeResponse에서 `CLIENT_SSL` 비트 설정 전송
4. auth 데이터 없이 순수 32B SSLRequest 패킷 전송
5. 서버가 SSLRequest 수신 후 프로토콜 전환 (이하 TLS 계층)
6. TLS 핸드셰이크 수행
7. TLS 위에서 actual HandshakeResponse41 전송 및 인증

이 프로토콜 흐름은 SQL Injection 탐지, 정책 검사 등이 **TLS 핸드셰이크 완료 후**에 수행되도록 설계되었다.

### 현재 구현 상태 (Phase 2)

**TCP 레벨 TLS만 구현 완료**:

- `Session::run()` (라인 838-938): Backend connect 직후 `ssl::stream`으로 즉시 업그레이드
- `strip_unsupported_capabilities()` (handshake.cpp 라인 125-168): 서버 greeting에서 `CLIENT_SSL` 제거
- `strip_unsupported_client_capabilities()` (handshake.cpp 라인 178-205): 클라이언트 응답에서 `CLIENT_SSL` 제거
- 결과: MySQL 클라이언트가 SSL 업그레이드를 시도하면 실패 (SSLRequest 미지원)

### 프로토콜 레벨 SSL 업그레이드 설계 (Phase 3 계획)

#### 예상 세션 흐름

```
Client                   Proxy                   MySQL Server
  |                        |                        |
  |                        |<-- Initial Handshake --|  (CLIENT_SSL capability 포함)
  |<-- relay greeting -----|                        |
  |--- SSLRequest(32B) --->|                        |  (CLIENT_SSL bit set, auth 데이터 없음)
  |      [TLS Handshake]   |                        |  (Frontend 측 TLS upgrade)
  |--- HandshakeResp41 --->|                        |  (이제부터 TLS 위에서 전송)
  |                        |--- SSLRequest(32B) --->|  (Proxy가 서버로 재전송)
  |                        |      [TLS Handshake]   |  (Backend 측 TLS upgrade)
  |                        |--- HandshakeResp41 --->|  (서버에 auth 데이터 전송)
  |                        |<-- OK/ERR/AuthSwitch --|
  |<-- relay --------------|                        |
```

#### 핵심 변경사항

1. **AsyncStream에 `upgrade_to_ssl()` 메서드** (계획):
   ```cpp
   // TCP 소켓을 in-place로 TLS stream으로 업그레이드
   auto upgrade_to_ssl(ssl::context& ctx, const std::string& sni_name)
       -> boost::asio::awaitable<std::expected<void, ParseError>>;
   ```
   - 평문 tcp::socket을 ssl::stream<tcp::socket>으로 변환
   - 기존 `std::visit` 기반 호출 유지로 투명성 보장

2. **HandshakeRelay 상태 머신 확장** (계획):
   - 기존: `kWaitServerGreeting` → `kWaitClientResponse` → ... → `kDone`
   - 새 상태: `kWaitClientSSLRequest` (CLIENT_SSL 선택 시)
   - SSLRequest 감지 후 `stream.upgrade_to_ssl()` 호출

3. **CLIENT_SSL 비트 조건부 유지** (계획):
   - Backend SSL 설정 활성화 시: `CLIENT_SSL` 비트 유지
   - Backend SSL 미설정 시: `CLIENT_SSL` 비트 제거 (기존 동작)
   - `Session::backend_ssl_ctx_`를 기준으로 판정

4. **신규 함수: SSLRequest 생성 및 검증** (계획):
   - SSLRequest는 fixed 32B: capability(4) + max_packet(4) + charset(1) + reserved(23)
   - auth_data/username/db_name 없음
   - Proxy가 양쪽 SSLRequest를 독립적으로 생성 가능

5. **Fail-close 원칙** (계획):
   - TLS 핸드셰이크 실패 시 세션 즉시 종료
   - 부분 업그레이드 상태에서 롤백 불가능

#### 단계적 구현 계획

**Phase 3.1**: `AsyncStream::upgrade_to_ssl()` 메서드 추가
- 호출: `stream.upgrade_to_ssl(ssl_ctx, sni_hostname)`
- 반환: `awaitable<expected<void, ParseError>>`
- 평문 모드에서만 호출 (이미 TLS면 오류)

**Phase 3.2**: HandshakeRelay에 SSL upgrade 상태 추가
- `detail::HandshakeState`에 `kWaitClientSSLRequest` 추가
- SSLRequest 패킷 감지 로직
- Frontend/Backend 간 독립적 upgrade

**Phase 3.3**: Session::run()에서 MySQL 프로토콜 기반 upgrade 처리
- Backend SSL 설정 시 handshake 루프 내에서 upgrade 트리거
- 기존 TCP 레벨 upgrade는 MySQL protocol level로 전환

**Phase 3.4**: 테스트 & 호환성 검증
- mysql-connector-python / JDBC / mysql-connector-java 호환성 검증
- `CLIENT_SSL` 비트 제거/유지 시나리오 테스트

### 제약사항 및 한계

1. **현재 미구현 (Phase 3 예정)**:
   - SSLRequest 패킷 파싱/생성
   - MySQL 프로토콜 레벨 upload 상태 머신
   - AsyncStream::upgrade_to_ssl()

2. **기존 기능과의 호환성**:
   - TCP 레벨 TLS (현재): 계속 지원
   - MySQL 프로토콜 레벨 TLS: 신규 모드 (향후)
   - 동시 활성화 불가 (둘 중 하나만 선택)

3. **준비 증거**:
   - ADR-007의 `AsyncStream` 설계가 protocol-agnostic 이므로 확장 가능
   - `Session::backend_ssl_ctx_` 존재로 backend SSL config 이미 지원

## References

### 구현 파일

- `src/common/async_stream.hpp` — AsyncStream 클래스 정의 (Phase 3에서 `upgrade_to_ssl()` 메서드 추가 예정)
- `src/common/async_stream.cpp` — AsyncStream 메서드 구현
- `src/protocol/handshake.hpp` — HandshakeRelay 인터페이스
- `src/protocol/handshake.cpp` (라인 125-168, 178-205) — 현재 CLIENT_SSL 비트 제거 로직 (Phase 3에서 조건부 유지로 변경 예정)
- `src/proxy/session.hpp` — Session 생성자/필드
- `src/proxy/session.cpp` (라인 838-938) — 현재 TCP 레벨 backend SSL 업그레이드 (Phase 3에서 MySQL protocol level로 전환 예정)
- `src/proxy/proxy_server.hpp` — ProxyConfig 구조 정의

### 관련 문서

- `docs/architecture.md` — SSL/TLS 아키텍처 다이어그램
- `docs/interface-reference.md` — HandshakeRelay 인터페이스 레퍼런스
- `CLAUDE.md` — C++ 아키텍처 규칙 (Module Dependency)

### MySQL 프로토콜 참고

- [MySQL 핸드셰이크 프로토콜](https://dev.mysql.com/doc/internals/en/connection-phase.html)
- `CLIENT_SSL` capability flag (0x0800)
- SSLRequest packet: 32 bytes fixed (capability + max_packet + charset + reserved, no auth data)

### Boost.Asio 참고

- `boost::asio::ip::tcp::socket`
- `boost::asio::ssl::stream<>`
- `boost::asio::async_result`, `boost::asio::use_awaitable`
- `std::variant`, `std::visit`
