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

## References

### 구현 파일

- `src/common/async_stream.hpp` — AsyncStream 클래스 정의
- `src/common/async_stream.cpp` — AsyncStream 메서드 구현
- `src/protocol/handshake.hpp` — HandshakeRelay 인터페이스 변경
- `src/proxy/session.hpp` — Session 생성자/필드 변경
- `src/proxy/proxy_server.hpp` — ProxyConfig 구조 정의 (frontend_ssl_*, backend_ssl_*, upstream_ssl_sni 필드)

### 관련 문서

- `docs/architecture.md` — SSL/TLS 아키텍처 다이어그램
- `docs/interface-reference.md` — HandshakeRelay 인터페이스 레퍼런스
- `CLAUDE.md` — C++ 아키텍처 규칙 (Module Dependency)

### Boost.Asio 참고

- `boost::asio::ip::tcp::socket`
- `boost::asio::ssl::stream<>`
- `boost::asio::async_result`, `boost::asio::use_awaitable`
- `std::variant`, `std::visit`
