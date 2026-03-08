# dbgate C++ 인터페이스 레퍼런스

## 개요

이 문서는 `src/` 하위 모든 헤더 파일의 공개 인터페이스를 정의합니다. 각 헤더는 담당 엔지니어가 Phase 2(인터페이스 정의)에서 확정했으며, **이후 변경은 architect 승인이 필수**입니다.

### CI 안정성 메모 (2026-03)

- 로깅 경로(`src/logger/**`)는 `info` 레벨 로그 버퍼링을 허용한다.
- 로그 파일 기반 검증(테스트/운영 스크립트)은 로거 종료 시점 flush 이후 결과를 읽어야 한다.
- `StructuredLogger` 소멸 경로는 종료 직전 flush를 보장한다.
- `Commit & PR Lint` 게이트의 제목/커밋 메시지 규칙은 다음 두 형식을 허용한다.
  - `type(scope): 설명`
  - `type(scope): 설명 [DON-XX]`

---

## 1. 공통 타입 (common/types.hpp)

### SessionContext

클라이언트 연결 하나를 식별하는 불변 컨텍스트입니다. proxy 레이어가 생성하고 parser/policy/logger 레이어에 const reference로 전달합니다.

```cpp
struct SessionContext {
    std::uint64_t session_id{0};
    std::string   client_ip{};
    std::uint16_t client_port{0};
    std::string   db_user{};
    std::string   db_name{};
    std::chrono::system_clock::time_point connected_at{};
    bool          handshake_done{false};
};
```

**멤버**:
- `session_id`: 프로세스 범위 내 유일 ID (proxy가 1부터 할당)
- `client_ip`: 클라이언트 IPv4/IPv6 (문자열)
- `client_port`: 클라이언트 TCP 포트
- `db_user`: MySQL 인증 사용자명
- `db_name`: 초기 접속 데이터베이스명
- `connected_at`: 연결 수립 시각
- `handshake_done`: 핸드셰이크 완료 여부

**사용처**: policy_engine, logger, stats

### ParseErrorCode

SQL 파싱 단계에서 발생 가능한 오류 분류입니다.

```cpp
enum class ParseErrorCode : std::uint8_t {
    kMalformedPacket    = 0,  // MySQL 패킷 구조 오류
    kInvalidSql         = 1,  // SQL 문법 오류
    kUnsupportedCommand = 2,  // 지원하지 않는 MySQL 커맨드
    kInternalError      = 3,  // 파서 내부 오류
};
```

### ParseError

파싱 실패 시 반환되는 오류 정보입니다. `std::expected<T, ParseError>` 패턴과 함께 사용합니다.

```cpp
struct ParseError {
    ParseErrorCode code{ParseErrorCode::kInternalError};
    std::string    message{};
    std::string    context{};  // 오류 발생 위치/입력 단편
};
```

### AsyncStream (common/async_stream.hpp)

TCP 소켓과 TLS(ssl::stream)을 투명하게 추상화하는 타입 소거 래퍼입니다. Frontend(클라이언트↔프록시)와 Backend(프록시↔MySQL) 양쪽에서 평문 또는 TLS 모드를 지원합니다.

#### 설계 원칙
- **통합 인터페이스**: tcp::socket과 ssl::stream<tcp::socket>을 std::variant로 관리
- **투명성**: 호출자는 TLS/평문을 구분하지 않고 같은 인터페이스 사용
- **이동 전용**: 복사 불가능, 소켓 소유권 명확성
- **비동기**: Boost.Asio awaitable 기반

#### AsyncStream 클래스

```cpp
class AsyncStream {
public:
    using executor_type = boost::asio::any_io_executor;
    using tcp_socket = boost::asio::ip::tcp::socket;
    using ssl_socket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

    // 생성자 — tcp::socket (평문 모드)
    explicit AsyncStream(tcp_socket socket);

    // 생성자 — ssl::stream<tcp::socket> (TLS 모드)
    explicit AsyncStream(ssl_socket ssl_stream);

    // 이동 생성자 / 이동 대입
    AsyncStream(AsyncStream&&) noexcept;
    AsyncStream& operator=(AsyncStream&&) noexcept;

    // 복사 금지
    AsyncStream(const AsyncStream&) = delete;
    AsyncStream& operator=(const AsyncStream&) = delete;

    ~AsyncStream();

    // Executor 획득 (Boost.Asio 호환)
    auto get_executor() -> executor_type;

    // 읽기 (std::visit으로 실제 스트림에 위임)
    template<typename MutableBufferSequence, typename ReadToken>
    auto async_read_some(const MutableBufferSequence& buffers, ReadToken&& token);

    // 쓰기
    template<typename ConstBufferSequence, typename WriteToken>
    auto async_write_some(const ConstBufferSequence& buffers, WriteToken&& token);

    // TLS 핸드셰이크
    // 평문 모드: no-op (boost::asio::post로 즉시 성공)
    // TLS 모드: ssl::stream::async_handshake 위임
    template<typename Token>
    auto async_handshake(boost::asio::ssl::stream_base::handshake_type type, Token&& token);

    // TLS 종료
    // 평문 모드: no-op
    // TLS 모드: ssl::stream::async_shutdown 위임
    template<typename Token>
    auto async_shutdown(Token&& token);

    // TCP 소켓 직접 접근 (connect/close/cancel/remote_endpoint 용)
    auto lowest_layer() -> tcp_socket&;

    // TLS 모드 여부 확인
    [[nodiscard]] bool is_ssl() const noexcept;

private:
    std::variant<tcp_socket, ssl_socket> stream_;
};
```

**사용 예시**:

```cpp
// 평문 모드 생성
AsyncStream stream{std::move(tcp_socket)};

// TLS 모드 생성
ssl::stream<tcp::socket> ssl_stream{std::move(tcp_socket), ssl_ctx};
AsyncStream stream{std::move(ssl_stream)};

// 공통 읽기 (co_await 사용)
auto bytes = co_await stream.async_read_some(buffer, use_awaitable);

// 공통 쓰기
co_await stream.async_write_some(buffer, use_awaitable);

// TLS 모드 핸드셰이크 (평문 모드: 즉시 성공)
co_await stream.async_handshake(ssl::stream_base::server, use_awaitable);

// TCP 소켓 직접 접근 (connect/close 등)
stream.lowest_layer().async_connect(endpoint, ...);
```

---

## 2. MySQL 프로토콜 (protocol/*)

### protocol/mysql_packet.hpp

MySQL 와이어 프로토콜의 단일 패킷을 나타냅니다.

#### PacketType

```cpp
enum class PacketType : std::uint8_t {
    kHandshake         = 0x0A,
    kHandshakeResponse = 0x00,
    kComQuery          = 0x03,
    kComQuit           = 0x01,
    kOk                = 0x00,
    kError             = 0xFF,
    kEof               = 0xFE,
    kUnknown           = 0x00,
};
```

#### MysqlPacket 클래스

```cpp
class MysqlPacket {
public:
    MysqlPacket() = default;

    // 파싱
    // data: MySQL 와이어 포맷 원시 바이트 (4byte 헤더 + 페이로드)
    // 반환: std::expected<MysqlPacket, ParseError>
    static auto parse(std::span<const std::uint8_t> data)
        -> std::expected<MysqlPacket, ParseError>;

    // Accessors
    [[nodiscard]] auto sequence_id()     const noexcept -> std::uint8_t;
    [[nodiscard]] auto payload_length()  const noexcept -> std::uint32_t;
    [[nodiscard]] auto payload()         const noexcept
        -> std::span<const std::uint8_t>;
    [[nodiscard]] auto type()            const noexcept -> PacketType;

    // 직렬화: 헤더 + 페이로드를 바이트 벡터로 반환
    [[nodiscard]] auto serialize() const -> std::vector<std::uint8_t>;

    // 팩토리: MySQL ERR Packet 생성
    // error_code: MySQL 오류 코드 (2바이트)
    // message: 사람이 읽을 수 있는 오류 메시지
    // sequence_id: 응답 시퀀스 ID
    static auto make_error(std::uint16_t   error_code,
                           std::string_view message,
                           std::uint8_t    sequence_id) -> MysqlPacket;
};
```

**설계 원칙**:
- 패킷 구조만 담당 (SQL 해석 안 함)
- span으로 복사 최소화
- expected<T, E> 패턴으로 에러 처리

---

### protocol/handshake.hpp

MySQL 핸드셰이크를 클라이언트 ↔ 서버 간 투명하게 릴레이합니다.

#### HandshakeRelay 클래스

```cpp
class HandshakeRelay {
public:
    HandshakeRelay() = default;

    // 핸드셰이크 수행 (AsyncStream 인터페이스)
    // client_stream: 클라이언트 측 AsyncStream (accept 된 소켓, 평문 또는 TLS)
    // server_stream: MySQL 서버 측 AsyncStream (connect 된 소켓, 평문 또는 TLS)
    // ctx: [out] db_user, db_name, handshake_done 이 채워짐
    // 반환: co_awaitable, 성공 시 expected<void, ParseError>
    static auto relay_handshake(
        AsyncStream&           client_stream,
        AsyncStream&           server_stream,
        SessionContext&        ctx
    ) -> boost::asio::awaitable<std::expected<void, ParseError>>;
};
```

**동작** (8-state 상태 머신):

#### 1단계: Initial Handshake (서버 → 클라이언트)
- 상태: `kWaitServerGreeting`
- 패킷 수신 (seq=0, type=0x0A)
- 클라이언트로 투명 전달 → 다음 상태: `kWaitClientResponse`
- 액션: `kRelayToClient`

#### 2단계: Handshake Response (클라이언트 → 서버)
- 상태: `kWaitClientResponse`
- 패킷 수신 (seq=1)
- 페이로드에서 username, database 필드 추출:
  - **Capability flags 읽기**: offset 0-3, 4byte LE
  - **Username**: offset 32 이후의 null-terminated string
  - **Auth Response 건너뜀**:
    - `CLIENT_PLUGIN_AUTH_LENENC (0x00200000)`: length-encoded (1/2/3바이트)
      - 0xFE/0xFF는 비정상 → ParseError
    - `CLIENT_SECURE_CONNECTION (0x00008000)`: 1바이트 length + 데이터
    - 없음: null-terminated
    - **모든 경우 남은 payload 검증**: auth_len > remaining → ParseError
  - **Database**: `CLIENT_CONNECT_WITH_DB (0x00000008)` 플래그 확인 후 추출
    - 플래그 설정 시 반드시 null-terminated string 필요 → 없으면 ParseError
    - 플래그 미설정 시 빈 문자열
- 서버로 투명 전달 → 다음 상태: `kWaitServerAuth`
- 액션: `kRelayToServer`

#### 3단계: Server Auth Response (서버 → 클라이언트)
- 상태: `kWaitServerAuth`
- 패킷 수신 (seq=2)
- 응답 타입 판단 (`classify_auth_response`):

| 응답 타입 | 패킷 첫바이트 | 조건 | 다음 상태 | 액션 |
|----------|-------------|-----|---------|------|
| **OK** | 0x00 | — | `kDone` | `kComplete` |
| **Error** | 0xFF | — | `kFailed` | `kTerminate` |
| **EOF** | 0xFE | payload < 9 | `kFailed` | `kTerminate` |
| **AuthSwitch** | 0xFE | payload >= 9 | `kWaitClientAuthSwitch` | `kRelayToClient` |
| **AuthMoreData** | 0x01 | — | `kWaitClientMoreData` | `kRelayToClient` |
| **Unknown** | 기타 | — | `kFailed` | `kTerminateNoRelay` |

#### AuthSwitch 라운드트립
- 상태: `kWaitClientAuthSwitch`
- 클라이언트 응답 → 서버 전달 → 다음 상태: `kWaitServerAuthSwitch`
- 액션: `kRelayToServer`

- 상태: `kWaitServerAuthSwitch`
- 서버 응답 분류:
  - **OK (0x00)**: 완료 → `kDone`, `kComplete`
  - **Error/EOF (0xFF/0xFE<9)**: 실패 → `kFailed`, `kTerminate`
  - **AuthMoreData (0x01)**: 라운드트립 체인 진입 (round_trips >= kMaxRoundTrips? → ParseError)
    - → `kWaitClientMoreData`, `kRelayToClient`, `round_trips++`
  - **AuthSwitch (0xFE≥9) 중첩**: ParseError (fail-close)
  - **Unknown**: → `kFailed`, `kTerminateNoRelay`

#### AuthMoreData 라운드트립 체인
- 상태: `kWaitClientMoreData`
- 클라이언트 응답 → 서버 전달 → 다음 상태: `kWaitServerMoreData`
- 액션: `kRelayToServer`

- 상태: `kWaitServerMoreData`
- 서버 응답 분류:
  - **OK (0x00)**: 완료 → `kDone`, `kComplete`
  - **Error/EOF (0xFF/0xFE<9)**: 실패 → `kFailed`, `kTerminate`
  - **AuthMoreData (0x01)**: 라운드트립 반복 (round_trips >= kMaxRoundTrips? → ParseError)
    - → `kWaitClientMoreData`, `kRelayToClient`, `round_trips++`
  - **AuthSwitch (0xFE≥9)**: ParseError (fail-close, AuthMoreData 중 AuthSwitch 금지)
  - **Unknown**: → `kFailed`, `kTerminateNoRelay`

#### 4단계: Completion (성공 시)
- 액션: `kComplete` 수행 시
- SessionContext 갱신:
  - `ctx.db_user` = extracted username
  - `ctx.db_name` = extracted database
  - `ctx.handshake_done = true`
- COM_QUERY 처리 루프 진입

**설계 원칙**:
- auth plugin 메커니즘에 개입 없음 (모든 플러그인 자동 지원)
- 핸드셰이크 패킷은 변조 없이 투명 릴레이
- 최소 파싱: Handshake Response의 기본 필드만 추출
- **AuthSwitch/AuthMoreData 대응**: 추가 라운드트립 자동 처리
- **최대 라운드트립 10회 제한** (kMaxRoundTrips=10): 무한 루프 방지
- **Unknown 패킷**: ERR 전달 없이 즉시 세션 종료 (fail-close)
- **AuthSwitch 중첩/순서 위반**: ParseError로 fail-close

**에러 처리** (fail-close):
- 패킷 읽기/쓰기 실패 → ParseError 반환 (세션 종료)
- 서버 ERR (0xFF) 또는 EOF (0xFE, payload<9) → 클라이언트로 전달 후 실패 반환
- Unknown 패킷 (0x00/0xFF/0xFE/0x01 이외) → `kTerminateNoRelay`: ERR 전달 없이 즉시 종료
- 라운드트립 10회 초과 → ParseError (세션 종료)
- Handshake Response 필드 오류 (최소 길이, null terminator, payload 초과) → ParseError
- 호출자는 반환값 확인 후 세션 종료

---

### protocol/command.hpp

handshake 이후 클라이언트가 보내는 커맨드를 추출합니다.

#### CommandType

```cpp
enum class CommandType : std::uint8_t {
    kComQuit        = 0x01,
    kComInitDb      = 0x02,  // USE database
    kComQuery       = 0x03,
    kComFieldList   = 0x04,
    kComCreateDb    = 0x05,
    kComDropDb      = 0x06,
    kComRefresh     = 0x07,
    kComStatistics  = 0x09,
    kComProcessInfo = 0x0A,
    kComConnect     = 0x0B,
    kComProcessKill = 0x0C,
    kComPing        = 0x0E,
    kComStmtPrepare = 0x16,  // COM_STMT_PREPARE (미파싱)
    kComStmtExecute = 0x17,  // COM_STMT_EXECUTE (미파싱)
    kComStmtClose   = 0x19,
    kComStmtReset   = 0x1A,
    kComUnknown     = 0xFF,
};
```

#### CommandPacket

```cpp
struct CommandPacket {
    CommandType  command_type{CommandType::kComUnknown};
    std::string  query{};        // COM_QUERY의 경우만 채워짐
    std::uint8_t sequence_id{0};
};
```

#### extract_command 함수

```cpp
// MysqlPacket 에서 CommandPacket 을 추출한다.
// 페이로드 첫 바이트를 CommandType으로 해석
// COM_QUERY이면 나머지를 query 문자열로 설정
auto extract_command(const MysqlPacket& packet)
    -> std::expected<CommandPacket, ParseError>;
```

**실패 조건**:
- 페이로드가 비어있음 → ParseErrorCode::kMalformedPacket
- 지원하지 않는 커맨드 → ParseErrorCode::kUnsupportedCommand

---

## 3. SQL 파서 (parser/*)

### parser/sql_parser.hpp

SQL 구문을 분류하고 테이블명을 추출하는 경량 파서입니다.

#### SqlCommand

첫 번째 키워드 기반 SQL 구문 분류입니다.

```cpp
enum class SqlCommand : std::uint8_t {
    kSelect   = 0,
    kInsert   = 1,
    kUpdate   = 2,
    kDelete   = 3,
    kDrop     = 4,
    kTruncate = 5,
    kAlter    = 6,
    kCreate   = 7,
    kCall     = 8,
    kPrepare  = 9,
    kExecute  = 10,
    kUnknown  = 11,  // 분류 불가 → 정책에서 차단
};
```

#### ParsedQuery

파싱 성공 시 반환되는 SQL 분석 결과입니다.

```cpp
struct ParsedQuery {
    SqlCommand               command{SqlCommand::kUnknown};
    std::vector<std::string> tables{};
    std::string              raw_sql{};
    bool                     has_where_clause{};
};
```

**멤버**:
- `command`: SQL 구문 종류
- `tables`: FROM/INTO/UPDATE/JOIN 뒤 추출한 테이블명 (기본적, 불완전 가능)
- `raw_sql`: 원문 SQL (로깅용, 변형 없음)
- `has_where_clause`: DELETE 무조건 삭제 탐지용

#### SqlParser 클래스

```cpp
class SqlParser {
public:
    SqlParser()  = default;
    ~SqlParser() = default;

    // Stateless이므로 복사/이동 허용
    SqlParser(const SqlParser&)            = default;
    SqlParser& operator=(const SqlParser&) = default;
    SqlParser(SqlParser&&)                 = default;
    SqlParser& operator=(SqlParser&&)      = default;

    // SQL을 파싱하여 ParsedQuery로 변환
    // 실패 시 std::unexpected(ParseError)
    [[nodiscard]] std::expected<ParsedQuery, ParseError>
    parse(std::string_view sql) const;
};
```

**설계 원칙**:
- 풀 파서가 아님 (키워드 분류 + 정규식)
- 파싱 실패는 fail-close (정책에서 BLOCK)
- 테이블명 추출이 불완전할 수 있음 (ORM 쿼리 등)

**한계**:
- 주석 분할: `DROP/**/TABLE` 탐지 불가
- 인코딩 우회: URL 인코딩, hex 리터럴 미지원
- 복잡한 서브쿼리: 내부 테이블명 추출 불완전

---

### parser/injection_detector.hpp

정규식 패턴 기반 SQL Injection 탐지입니다.

#### InjectionResult

```cpp
struct InjectionResult {
    bool        detected{false};
    std::string matched_pattern{};   // 로깅/감사 목적
    std::string reason{};            // 사람이 읽을 수 있는 설명
};
```

#### InjectionDetector 클래스

```cpp
class InjectionDetector {
public:
    // 생성자: 정규식 패턴을 컴파일
    // patterns: "' OR 1=1 --", "UNION SELECT", "SLEEP(", ...
    // 잘못된 패턴은 로그하고 건너뜀 (fail-open 방지)
    explicit InjectionDetector(std::vector<std::string> patterns);

    ~InjectionDetector() = default;

    // 복사 금지 (regex 재사용 비용), 이동 허용
    InjectionDetector(const InjectionDetector&)            = delete;
    InjectionDetector& operator=(const InjectionDetector&) = delete;
    InjectionDetector(InjectionDetector&&)                 = default;
    InjectionDetector& operator=(InjectionDetector&&)      = default;

    // SQL 검사: 패턴 매칭
    [[nodiscard]] InjectionResult check(std::string_view sql) const;
};
```

**특징**:
- Regex 컴파일이 생성자에서 발생 (재사용 권장)
- O(P × N) 복잡도 (P = 패턴, N = SQL 길이)
- 잘못된 정규식 패턴은 경고 로그 후 건너뜀 (나머지 패턴은 계속 적용)
- **패턴 목록이 비어있거나 모든 패턴이 유효하지 않으면 fail-close — 모든 SQL이 차단됨**

**기본 탐지 패턴 (10가지)**:
1. `UNION\s+SELECT` — UNION 기반 인젝션
2. `'\s*OR\s+['"\d]` — tautology (OR 기반 Boolean blind)
3. `SLEEP\s*\(` — time-based blind
4. `BENCHMARK\s*\(` — time-based blind
5. `LOAD_FILE\s*\(` — 파일 읽기
6. `INTO\s+OUTFILE` — 파일 쓰기
7. `INTO\s+DUMPFILE` — 파일 덤프
8. `;\s*(DROP|DELETE|UPDATE|INSERT|ALTER|CREATE)` — piggyback 공격
9. `--\s*$` — 주석 꼬리 무력화
10. `/\*.*\*/` — 인라인 주석 우회

**한계**:
- 주석 분할: `UN/**/ION` 미탐지 (전처리 단계 없음)
- 인코딩 우회: URL 인코딩, hex 리터럴 미지원
- 오탐: ORM 생성 쿼리에서 UNION false positive 가능
- OR 조건: `OR true` 등 다른 형태 미탐지 (패턴 2는 따옴표/숫자 시작으로 제한)

---

### parser/procedure_detector.hpp

CALL/PREPARE/EXECUTE 등 프로시저 관련 구문을 탐지합니다.

#### ProcedureType

```cpp
enum class ProcedureType : std::uint8_t {
    kCall             = 0,
    kCreateProcedure  = 1,
    kAlterProcedure   = 2,
    kDropProcedure    = 3,
    kPrepareExecute   = 4,  // PREPARE 또는 EXECUTE
};
```

#### ProcedureInfo

프로시저 탐지 결과입니다.

```cpp
struct ProcedureInfo {
    ProcedureType type{ProcedureType::kCall};
    std::string   procedure_name{};      // CALL에서만 유효
    bool          is_dynamic_sql{false}; // PREPARE/EXECUTE 여부
};
```

#### ProcedureDetector 클래스

```cpp
class ProcedureDetector {
public:
    ProcedureDetector()  = default;
    ~ProcedureDetector() = default;

    // Stateless이므로 복사/이동 허용
    ProcedureDetector(const ProcedureDetector&)            = default;
    ProcedureDetector& operator=(const ProcedureDetector&) = default;
    ProcedureDetector(ProcedureDetector&&)                 = default;
    ProcedureDetector& operator=(ProcedureDetector&&)      = default;

    // ParsedQuery에서 프로시저/동적 SQL 정보 추출
    // 해당 없으면 nullopt
    [[nodiscard]] std::optional<ProcedureInfo>
    detect(const ParsedQuery& query) const;
};
```

**설계 원칙**:
- ParsedQuery를 입력 (sql_parser와 협력)
- 프로시저 이름은 CALL의 경우만 추출
- is_dynamic_sql=true이면 정책 엔진에서 추가 검사

**한계**:
- 변수 간접 참조: `SET @q = '...'; PREPARE FROM @q;` 미탐지
- 다중 구문: 첫 번째 구문만 분류

---

## 4. 정책 엔진 (policy/*)

### policy/rule.hpp

정책 설정 구조체 정의입니다. YAML에서 로드되어 PolicyEngine이 참조합니다.

#### TimeRestriction

시간대별 접근 제어 설정입니다.

```cpp
struct TimeRestriction {
    std::string allow_range{"09:00-18:00"};  // "HH:MM-HH:MM" 형식
    std::string timezone{"UTC"};             // IANA timezone ID
};
```

#### AccessRule

사용자/IP 기반 접근 제어 규칙입니다.

```cpp
struct AccessRule {
    std::string              user{};
    std::string              source_ip_cidr{};      // CIDR 표기법
    std::vector<std::string> allowed_tables{"*"};
    std::vector<std::string> allowed_operations{};
    std::vector<std::string> blocked_operations{};  // 우선
    std::optional<TimeRestriction> time_restriction{};
};
```

**주의**:
- `allowed_tables = ["*"]`: 모든 테이블 접근 허용
- `blocked_operations`이 `allowed_operations`보다 우선
- IP 스푸핑 방어는 네트워크 레이어 책임

#### SqlRule

SQL 구문 레벨 차단 규칙입니다.

```cpp
struct SqlRule {
    std::vector<std::string> block_statements{};  // "DROP", "TRUNCATE"
    std::vector<std::string> block_patterns{};    // 정규식 패턴
};
```

#### ProcedureControl

프로시저 제어 설정입니다.

```cpp
struct ProcedureControl {
    std::string              mode{"whitelist"};      // "whitelist" | "blacklist"
    std::vector<std::string> whitelist{};
    bool                     block_dynamic_sql{true};
    bool                     block_create_alter{true};
};
```

#### DataProtection

데이터 유출 방지 설정입니다.

```cpp
struct DataProtection {
    std::uint32_t max_result_rows{0};           // 0 = 제한 없음
    bool          block_schema_access{true};
};
```

#### GlobalConfig

전역 설정값입니다.

```cpp
struct GlobalConfig {
    std::string   log_level{"info"};       // "trace"|"debug"|"info"|"warn"|"error"
    std::string   log_format{"json"};
    std::uint32_t max_connections{1000};
    std::uint32_t connection_timeout_sec{30};
};
```

#### PolicyConfig

전체 정책 설정의 루트 구조체입니다.

```cpp
struct PolicyConfig {
    GlobalConfig             global{};
    std::vector<AccessRule>  access_control{};
    SqlRule                  sql_rules{};
    ProcedureControl         procedure_control{};
    DataProtection           data_protection{};
};
```

---

### policy/policy_loader.hpp

YAML 정책 파일을 로드하고 Hot Reload를 지원합니다.

#### PolicyLoader 클래스

```cpp
class PolicyLoader {
public:
    using ReloadCallback = std::function<void(std::shared_ptr<PolicyConfig>)>;

    PolicyLoader()  = default;
    ~PolicyLoader() = default;

    PolicyLoader(const PolicyLoader&)            = default;
    PolicyLoader& operator=(const PolicyLoader&) = default;
    PolicyLoader(PolicyLoader&&)                 = default;
    PolicyLoader& operator=(PolicyLoader&&)      = default;

    // YAML 파일 로드
    // 성공: PolicyConfig
    // 실패: 오류 메시지
    // 실패 시 호출자는 기존 정책 유지 또는 차단 처리 필수
    [[nodiscard]] static std::expected<PolicyConfig, std::string>
    load(const std::filesystem::path& config_path);

    // Phase 4: Hot Reload (watch 기능 선언만, 미구현)
    // using ReloadCallback = std::function<void(std::shared_ptr<PolicyConfig>)>;
};
```

**설계 원칙**:
- 로드 실패 시 명시적 fail-close (차단 처리)
- 파싱 실패 시 부분 정책 반환 금지 (all-or-nothing)
- 경로는 config에서만 지정 (사용자 입력 직접 사용 금지)

---

### policy/policy_version_store.hpp (DON-50)

정책 스냅샷의 저장/로드/목록/정리를 담당하는 컴포넌트입니다.

#### PolicyVersionMeta

저장된 정책 스냅샷의 메타데이터입니다.

```cpp
struct PolicyVersionMeta {
    std::uint64_t version{0};                      // 단조 증가 버전 번호 (1부터 시작)
    std::string timestamp{};                       // ISO 8601 UTC (예: "20260304T103000Z")
    std::uint32_t rules_count{0};                  // 해당 정책의 access_control 규칙 수
    std::string hash{};                            // SHA-256 hex digest (파일 무결성)
    std::filesystem::path snapshot_path{};         // 저장된 스냅샷 파일의 절대 경로
};
```

**멤버**:
- `version`: 버전 번호 (PolicyEngine::current_version()과 일치)
- `timestamp`: 저장 시각 (ISO 8601 compact UTC)
- `rules_count`: 정책의 access_control 규칙 수
- `hash`: 원본 YAML 파일의 SHA-256 hex digest (무결성 확인용)
- `snapshot_path`: 스냅샷 파일이 저장된 경로 (`.policy_versions/v{version}_{timestamp}.yaml`)

#### PolicyVersionStore 클래스

```cpp
class PolicyVersionStore {
public:
    // 생성자
    // config_dir : 정책 설정 디렉토리 (스냅샷은 config_dir/.policy_versions/ 에 저장)
    // max_versions: 유지할 최대 스냅샷 수 (초과 시 오래된 것부터 자동 삭제)
    explicit PolicyVersionStore(const std::filesystem::path& config_dir,
                                std::uint32_t max_versions = 10);

    ~PolicyVersionStore() = default;

    PolicyVersionStore(const PolicyVersionStore&) = delete;
    PolicyVersionStore& operator=(const PolicyVersionStore&) = delete;
    PolicyVersionStore(PolicyVersionStore&&) = delete;
    PolicyVersionStore& operator=(PolicyVersionStore&&) = delete;

    // save_snapshot
    //   source_path 의 YAML 파일을 스냅샷 디렉토리에 복사하여 버전을 저장한다.
    //
    //   성공: PolicyVersionMeta 반환 (version, timestamp, hash, snapshot_path 포함)
    //   실패: std::unexpected(error_message) 반환
    //   fail-close: 스냅샷 저장 실패해도 현재 정책에 영향 없음
    [[nodiscard]] std::expected<PolicyVersionMeta, std::string> save_snapshot(
        const PolicyConfig& config,
        const std::filesystem::path& source_path);

    // load_snapshot
    //   version 에 해당하는 스냅샷을 파일에서 읽어 PolicyConfig 로 반환한다.
    //
    //   성공: PolicyConfig 반환
    //   실패: std::unexpected(error_message) 반환 (파일 없음, 파싱 오류, 버전 미발견)
    //   fail-close: 호출자는 실패 시 기존 정책 유지 또는 차단 처리
    [[nodiscard]] std::expected<PolicyConfig, std::string> load_snapshot(
        std::uint64_t version) const;

    // list_versions
    //   저장된 모든 버전의 메타데이터를 반환한다 (최신 버전 먼저 정렬).
    [[nodiscard]] std::vector<PolicyVersionMeta> list_versions() const;

    // current_version
    //   마지막으로 저장된 버전 번호를 반환한다.
    //   저장된 버전이 없으면 0 을 반환한다.
    [[nodiscard]] std::uint64_t current_version() const;
};
```

**설계 원칙**:
- **원본 파일 복사 방식**: YAML 재직렬화 없이 원본 파일을 스냅샷으로 복사
- **SHA-256 무결성 추적**: OpenSSL EVP API로 원본 파일의 해시를 계산하여 저장
- **자동 정리(prune)**: `max_versions` 초과 시 오래된 스냅샷을 자동 삭제
- **스레드 안전성**: 모든 공개 메서드는 std::mutex로 직렬화
- **파일 구조**: `config_dir/.policy_versions/v{version}_{iso_timestamp}.yaml`

**fail-close 연계**:
- `load_snapshot` 실패 시 std::unexpected 반환 → 호출자가 기존 정책 유지 책임
- `save_snapshot` 실패는 데이터패스에 영향 없음 (스냅샷 미보관일 뿐)

**알려진 한계**:
| 항목 | 설명 |
|------|------|
| 디렉토리 외부 수정 | `.policy_versions/` 외부에서 파일이 추가/삭제되면 내부 목록과 불일치 발생. 재시작 시 디렉토리 스캔으로 복원됨. |
| 복원 시 rules_count/hash | 재시작 후 스캔 복원 시 `rules_count=0`, `hash=""` 로 초기화됨. |
| 타임스탬프 동시성 | 동일 초에 여러 번 저장 시 파일명 충돌 가능 (`overwrite_existing` 옵션으로 덮어씀). |

---

### policy/policy_engine.hpp

파싱된 쿼리와 세션 컨텍스트를 받아 ALLOW/BLOCK/LOG 판정을 내립니다.

#### PolicyAction

정책 평가 결과 액션입니다.

```cpp
enum class PolicyAction : std::uint8_t {
    kAllow = 0,  // 명시적 allow 규칙 일치 시만
    kBlock = 1,  // default deny 또는 명시적 block 규칙 일치
    kLog   = 2,  // 허용하되 감사 로그 기록 (monitor mode로 다운그레이드된 차단)
};
```

**각 액션의 의미**:
- `kAllow`: 쿼리 허용 (명시적 allow 규칙이 모든 단계를 통과했을 때만)
- `kBlock`: 쿼리 차단 (정책에 불일치하거나 차단 규칙 일치)
- `kLog`: 쿼리 허용 + 감사 로그 (monitor mode 규칙에 의해 kBlock이 kLog로 다운그레이드됨, DON-49)

#### PolicyResult

정책 평가 결과입니다.

```cpp
struct PolicyResult {
    PolicyAction action{PolicyAction::kBlock};  // 기본값 BLOCK
    std::string  matched_rule{};                // 규칙 ID
    std::string  reason{};                      // 판정 이유
    bool monitor_mode{false};                   // true: monitor 모드에 의해 kBlock→kLog 다운그레이드됨
};
```

**멤버**:
- `action`: 판정 결과 (kAllow/kBlock/kLog)
- `matched_rule`: 판정에 사용된 규칙 식별자 (없으면 "default-deny")
- `reason`: 판정 이유 (로깅용, 클라이언트 직접 노출 비권장)
- `monitor_mode`: Monitor mode 규칙에 의해 차단이 로그로만 기록되는지 여부

#### ExplainResult (DON-48)

정책 평가 dry-run 결과입니다 (디버깅/감사 전용, 실제 차단 수행 없음).

```cpp
struct ExplainResult {
    PolicyAction action{PolicyAction::kBlock};     // 기본값 BLOCK (fail-close)
    std::string matched_rule{};                    // 매칭된 규칙 식별자
    std::string reason{};                          // 판정 이유 (로깅용)
    std::string matched_access_rule{};             // "user@cidr" 형식, access rule 매칭 시만 채움
    std::string evaluation_path{};                 // 단계별 평가 경로 trace
    bool monitor_mode{false};                      // true: monitor 모드에 의해 kBlock→kLog 다운그레이드됨
};
```

**멤버**:
- `action`: 판정 결과 (kAllow/kBlock/kLog)
- `matched_rule`: 판정에 사용된 규칙 식별자
- `reason`: 판정 이유 (로깅용, 클라이언트 노출 금지)
- `matched_access_rule`: access_control 룰 매칭 시 `"user@cidr"` 형식. 매칭 없으면 빈 문자열
- `evaluation_path`: 단계별 평가 경로 추적 (예: `"config_loaded > access_rule_matched(admin@10.0.0.0/8) > command_allowed(SELECT)"`)
- `monitor_mode`: Monitor mode 규칙에 의한 다운그레이드 여부

**주의**:
- explain()은 **디버깅/감사 목적 전용**이며, 프로덕션 데이터패스에서는 evaluate()를 사용해야 한다.
- evaluation_path와 matched_access_rule은 정책 매칭 경로를 노출하므로 권한이 있는 사용자에게만 공개한다.

#### PolicyEngine 클래스

```cpp
class PolicyEngine {
public:
    // config가 nullptr이면 모든 evaluate()가 kBlock 반환 (fail-close)
    explicit PolicyEngine(std::shared_ptr<PolicyConfig> config);

    ~PolicyEngine() = default;

    PolicyEngine(const PolicyEngine&)            = delete;
    PolicyEngine& operator=(const PolicyEngine&) = delete;
    PolicyEngine(PolicyEngine&&)                 = default;
    PolicyEngine& operator=(PolicyEngine&&)      = default;

    // evaluate
    //   파싱된 쿼리와 세션 컨텍스트를 기반으로 정책을 평가한다.
    //   평가 순서 (반드시 준수):
    //   1. SQL 구문 차단 (block_statements)
    //   2. SQL 패턴 차단 (block_patterns / InjectionDetector)
    //   3. 사용자/IP 접근 제어
    //   4. 테이블 접근 제어
    //   5. 시간대 제한
    //   6. 프로시저 제어
    //   7. 명시적 allow → kAllow
    //   8. 일치 없음 → kBlock (default deny)
    [[nodiscard]] PolicyResult evaluate(
        const ParsedQuery&     query,
        const SessionContext&  session) const;

    // evaluate_error
    //   파서 오류 발생 시 호출. 반드시 PolicyAction::kBlock을 반환 (fail-close).
    //   어떠한 경우에도 kAllow 또는 kLog를 반환해서는 안 됨 (noexcept).
    [[nodiscard]] PolicyResult evaluate_error(
        const ParseError&     error,
        const SessionContext& session) const noexcept;

    // explain (DON-48)
    //   evaluate()와 동일한 결정 로직을 따르되, 실제 차단/로깅 없이
    //   ExplainResult만 반환한다 (dry-run 용도, 디버깅/감사 전용).
    //   [주의] 프로덕션 데이터패스에서는 evaluate()를 사용해야 한다.
    [[nodiscard]] ExplainResult explain(const ParsedQuery& query,
                                        const SessionContext& session) const;

    // explain_error (DON-48)
    //   ParseError를 받아 ExplainResult를 반환한다.
    //   evaluate_error()와 동일하게 반드시 action=kBlock 반환 (fail-close).
    [[nodiscard]] ExplainResult explain_error(const ParseError& error,
                                              const SessionContext& session) const noexcept;

    // Hot Reload: 새 정책으로 원자적 교체
    // 진행 중인 evaluate()는 이전 config로 완료
    // 새로운 evaluate()는 new_config 사용
    // 이 오버로드는 version=0으로 버전 추적을 생략 (하위 호환)
    void reload(std::shared_ptr<PolicyConfig> new_config);

    // reload (버전 추적 오버로드, DON-50)
    //   new_config 교체 후 current_version_을 version으로 갱신한다.
    //   기존 reload(shared_ptr) 시그니처와 하위 호환 유지.
    void reload(std::shared_ptr<PolicyConfig> new_config, std::uint64_t version);

    // current_version (DON-50)
    //   현재 활성 정책의 버전 번호를 반환한다 (noexcept).
    //   reload(config, version)으로 설정된 버전을 반환.
    //   reload(config) (버전 없음) 호출 시 0으로 리셋됨.
    [[nodiscard]] std::uint64_t current_version() const noexcept;
};
```

**Fail-Close 보장**:
- 파서 오류 → evaluate_error() → kBlock
- 정책 일치 없음 → kBlock (default deny)
- 엔진 오류 → kBlock
- kAllow는 명시적 allow 규칙 필요

**thread-safety**:
- evaluate/evaluate_error/explain/explain_error: 읽기 전용, concurrent 호출 안전
- reload: std::atomic<std::shared_ptr<>>으로 원자적 교체, data race 없음

---

## 5. 로깅 (logger/*)

### logger/log_types.hpp

로거가 사용하는 구조화 로그 타입 정의입니다.

#### LogLevel

```cpp
enum class LogLevel : std::uint8_t {
    kDebug = 0,
    kInfo  = 1,
    kWarn  = 2,
    kError = 3,
};
```

#### ConnectionLog

클라이언트 연결/해제 이벤트 로그입니다.

```cpp
struct ConnectionLog {
    std::uint64_t                         session_id{0};
    std::string                           event{};        // "connect" | "disconnect"
    std::string                           client_ip{};
    std::uint16_t                         client_port{0};
    std::string                           db_user{};
    std::chrono::system_clock::time_point timestamp{};
};
```

#### QueryLog

SQL 쿼리 실행 로그입니다.

```cpp
struct QueryLog {
    std::uint64_t                         session_id{0};
    std::string                           db_user{};
    std::string                           client_ip{};
    std::string                           raw_sql{};      // 마스킹 주의
    std::uint8_t                          command_raw{0}; // SqlCommand as uint8_t
    std::vector<std::string>              tables{};
    std::uint8_t                          action_raw{0};  // PolicyAction as uint8_t
    std::chrono::system_clock::time_point timestamp{};
    std::chrono::microseconds             duration{0};
};
```

**주의**:
- `command_raw`: 호출자가 `static_cast<uint8_t>(parsed_query.command)` 수행
- `action_raw`: 호출자가 `static_cast<uint8_t>(policy_result.action)` 수행
- `raw_sql`: 원문 SQL (민감정보 포함 가능, 운영 환경에서 마스킹 고려)

#### BlockLog

차단 이벤트 로그입니다.

```cpp
struct BlockLog {
    std::uint64_t                         session_id{0};
    std::string                           db_user{};
    std::string                           client_ip{};
    std::string                           raw_sql{};      // 마스킹 주의
    std::string                           matched_rule{}; // 규칙 ID
    std::string                           reason{};       // 차단 사유
    std::chrono::system_clock::time_point timestamp{};
};
```

---

### logger/structured_logger.hpp

spdlog 기반 구조화 JSON 로거입니다.

#### StructuredLogger 클래스

```cpp
class StructuredLogger {
public:
    // 생성자
    // min_level: 이 레벨 미만의 로그는 기록하지 않음
    // log_path: 로그 파일 경로 (디렉터리 아님)
    explicit StructuredLogger(LogLevel min_level,
                              const std::filesystem::path& log_path);

    ~StructuredLogger() = default;

    StructuredLogger(const StructuredLogger&)            = delete;
    StructuredLogger& operator=(const StructuredLogger&) = delete;
    StructuredLogger(StructuredLogger&&)                 = default;
    StructuredLogger& operator=(StructuredLogger&&)      = default;

    // 연결/해제 이벤트 로깅
    void log_connection(const ConnectionLog& entry);

    // 쿼리 로깅 (고빈도, 문자열 복사 최소화)
    void log_query(const QueryLog& entry);

    // 차단 이벤트 로깅
    void log_block(const BlockLog& entry);

    // 내부 진단용 spdlog 래퍼
    void debug(std::string_view msg);
    void info(std::string_view msg);
    void warn(std::string_view msg);
    void error(std::string_view msg);
};
```

**설계 원칙**:
- 고빈도 경로(log_query)에서 const-ref 사용
- 로깅 실패가 데이터패스로 전파되지 않도록 설계
- JSON 스키마 일관성 유지

---

## 6. 통계 (stats/*)

### stats/stats_collector.hpp

실시간 통계를 원자적으로 수집합니다 (lock-free).

#### StatsSnapshot

특정 시점의 통계 스냅샷입니다.

```cpp
struct StatsSnapshot {
    std::uint64_t                         total_connections{0};
    std::uint64_t                         active_sessions{0};
    std::uint64_t                         total_queries{0};
    std::uint64_t                         blocked_queries{0};
    double                                qps{0.0};           // 1초 슬라이딩 윈도우
    double                                block_rate{0.0};    // blocked/total
    std::chrono::system_clock::time_point captured_at{};
};
```

#### StatsCollector 클래스

```cpp
class StatsCollector {
public:
    StatsCollector() noexcept;
    ~StatsCollector() = default;

    StatsCollector(const StatsCollector&)            = delete;
    StatsCollector& operator=(const StatsCollector&) = delete;
    StatsCollector(StatsCollector&&)                 = delete;
    StatsCollector& operator=(StatsCollector&&)      = delete;

    // 데이터패스 메서드 (고빈도, noexcept)
    void on_connection_open() noexcept;
    void on_connection_close() noexcept;
    void on_query(bool blocked) noexcept;

    // 조회 경로 메서드
    // 뮤텍스 없이 atomic 로드로 스냅샷 반환
    [[nodiscard]] StatsSnapshot snapshot() const noexcept;
};
```

**특징**:
- 모든 메서드 noexcept (데이터패스 오류 격리)
- Atomic 기반 (뮤텍스 없음)
- memory_order_relaxed로 최적화
- QPS는 슬라이딩 윈도우 (Phase 3 1초 윈도우로 개선 예정)

---

### stats/uds_server.hpp

Unix Domain Socket 서버로 Go CLI에 통계를 노출합니다.

#### UdsServer 클래스

```cpp
class UdsServer {
public:
    // 생성자 1: stats 전용 (policy_explain 비활성)
    UdsServer(const std::filesystem::path&    socket_path,
              std::shared_ptr<StatsCollector> stats,
              asio::io_context&               ioc);

    // 생성자 2: policy_explain 활성
    UdsServer(const std::filesystem::path&    socket_path,
              std::shared_ptr<StatsCollector> stats,
              std::shared_ptr<PolicyEngine>   policy_engine,
              std::shared_ptr<SqlParser>      sql_parser,
              asio::io_context&               ioc);

    // 생성자 3: policy_versions/policy_rollback/policy_reload 활성 (DON-50)
    UdsServer(const std::filesystem::path&      socket_path,
              std::shared_ptr<StatsCollector>   stats,
              std::shared_ptr<PolicyEngine>     policy_engine,
              std::shared_ptr<SqlParser>        sql_parser,
              std::shared_ptr<PolicyVersionStore> version_store,
              const std::filesystem::path&      policy_config_path,
              asio::io_context&                 ioc);

    ~UdsServer();

    UdsServer(const UdsServer&)            = delete;
    UdsServer& operator=(const UdsServer&) = delete;
    UdsServer(UdsServer&&)                 = delete;
    UdsServer& operator=(UdsServer&&)      = delete;

    // UDS 소켓 바인드/리슨 후 accept 루프 실행
    // io_context 스레드에서 co_spawn되어야 함
    asio::awaitable<void> run();

    // Acceptor 닫기 (run()의 accept 루프 종료)
    void stop();
};
```

**프로토콜**:
- 요청: `[4byte LE 길이][JSON]`
  - 예: `{"command": "stats", "version": 1}`
- 응답: `[4byte LE 길이][JSON]`
  - 성공: `{"ok": true, "payload": {...}}`
  - 실패: `{"ok": false, "error": "<msg>"}`

**지원 커맨드** (버전에 따라 활성화):
- `"stats"`: StatsSnapshot 반환 (생성자 1/2/3)
- `"policy_explain"`: SQL 정책 평가 dry-run (생성자 2/3, DON-48)
- `"policy_versions"`: 저장된 정책 버전 목록 조회 (생성자 3, DON-50)
- `"policy_rollback"`: 특정 버전으로 정책 롤백 (생성자 3, DON-50)
- `"policy_reload"`: 정책 파일 리로드 + 스냅샷 저장 (생성자 3, DON-50)
- `"sessions"`: 활성 세션 목록 (Phase 3 확장 예정)

---

## 7. 헬스체크 (health/*)

### health/health_check.hpp

HTTP 헬스체크 엔드포인트입니다.

#### HealthStatus

```cpp
enum class HealthStatus : std::uint8_t {
    kHealthy   = 0,  // HTTP 200 + {"status":"ok"}
    kUnhealthy = 1,  // HTTP 503 + {"status":"unhealthy","reason":"..."}
};
```

#### HealthCheck 클래스

```cpp
class HealthCheck {
public:
    HealthCheck(std::uint16_t                   port,
                std::shared_ptr<StatsCollector> stats,
                boost::asio::io_context&        io_context);

    ~HealthCheck() = default;

    HealthCheck(const HealthCheck&)            = delete;
    HealthCheck& operator=(const HealthCheck&) = delete;
    HealthCheck(HealthCheck&&)                 = delete;
    HealthCheck& operator=(HealthCheck&&)      = delete;

    // HTTP 서버 시작 (포트 바인드 → accept 루프)
    auto run() -> boost::asio::awaitable<void>;

    // 상태를 unhealthy로 설정 (로드밸런서 연동)
    void set_unhealthy(std::string_view reason);

    // 상태를 healthy로 복구
    void set_healthy();

    // 현재 상태 조회
    [[nodiscard]] auto status() const noexcept -> HealthStatus;
};
```

**응답 포맷**:
- GET /health (200): `{"status":"ok"}`
- GET /health (503): `{"status":"unhealthy","reason":"<reason>"}`

---

## 8. 프록시 (proxy/*)

### proxy/proxy_server.hpp

TCP 리슨 → 세션 생성 → Graceful Shutdown을 담당하는 메인 서버입니다.

#### ProxyConfig

```cpp
struct ProxyConfig {
    std::string   listen_address{};
    std::uint16_t listen_port{0};

    std::string   upstream_address{};
    std::uint16_t upstream_port{0};

    std::uint32_t max_connections{0};
    std::uint32_t connection_timeout_sec{0};

    std::string   policy_path{};
    std::string   uds_socket_path{};
    std::string   log_path{};
    std::string   log_level{};

    std::uint16_t health_check_port{0};

    // SSL/TLS 설정 (DON-31) — Frontend (클라이언트↔프록시)
    bool        frontend_ssl_enabled{false};
    std::string frontend_ssl_cert_path{};      // TLS 인증서 파일 경로 (.pem)
    std::string frontend_ssl_key_path{};       // TLS 개인키 파일 경로 (.pem)

    // SSL/TLS 설정 (DON-31) — Backend (프록시↔MySQL)
    bool        backend_ssl_enabled{false};
    std::string backend_ssl_ca_path{};         // CA 인증서 파일 경로 (서버 검증용)
    bool        backend_ssl_verify{true};      // 서버 인증서 검증 여부
    std::string upstream_ssl_sni{};            // SNI 호스트명 (backend에서 사용)
};
```

**주의**: 모든 값은 config 파일 또는 환경변수에서 로드 (하드코딩 금지)

**SSL 설정 유효성**:
- `frontend_ssl_enabled=true` 시:
  - `frontend_ssl_cert_path`, `frontend_ssl_key_path` 필수 (빈 문자열 불가)
  - 파일 읽기/파싱 실패 → fail-close
- `backend_ssl_enabled=true` 시:
  - `backend_ssl_verify=true` 시: `backend_ssl_ca_path` 필수
  - 파일 읽기/파싱 실패 → fail-close
- 위반 시: ProxyServer::init_ssl() 반환 false → 서버 기동 실패

#### ProxyServer 클래스

```cpp
class ProxyServer {
public:
    explicit ProxyServer(ProxyConfig config);
    ~ProxyServer() = default;

    ProxyServer(const ProxyServer&)            = delete;
    ProxyServer& operator=(const ProxyServer&) = delete;
    ProxyServer(ProxyServer&&)                 = delete;
    ProxyServer& operator=(ProxyServer&&)      = delete;

    // Accept 루프 시작
    // 호출자가 io_context.run()을 실행해야 함
    void run(boost::asio::io_context& io_ctx);

    // Graceful Shutdown
    // - 새 연결 accept 중단
    // - 진행 중인 세션 완료 대기
    // - 모든 세션 종료 후 io_context 중단
    void stop();
};
```

---

### proxy/session.hpp

클라이언트 1개와 MySQL 서버 1개를 1:1로 릴레이하는 세션입니다.

#### SessionState

```cpp
enum class SessionState : std::uint8_t {
    kHandshaking     = 0,  // 핸드셰이크 진행 중
    kReady           = 1,  // 핸드셰이크 완료, 커맨드 대기
    kProcessingQuery = 2,  // COM_QUERY 처리 중
    kClosing         = 3,  // Graceful Shutdown 진행 중
    kClosed          = 4,  // 세션 완전 종료
};
```

#### Session 클래스

```cpp
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(std::uint64_t                      session_id,
            AsyncStream                        client_stream,
            boost::asio::ip::tcp::endpoint     server_endpoint,
            boost::asio::ssl::context*         backend_ssl_ctx,
            std::shared_ptr<PolicyEngine>      policy,
            std::shared_ptr<StructuredLogger>  logger,
            std::shared_ptr<StatsCollector>    stats);

    ~Session() = default;

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&)                 = delete;
    Session& operator=(Session&&)      = delete;

    // 세션 메인 코루틴
    // 핸드셰이크 → 커맨드 루프 → 정리 순서로 동작
    auto run() -> boost::asio::awaitable<void>;

    // 정상 종료 (kClosing → kClosed)
    // 진행 중인 쿼리는 완료 후 종료
    void close();

    // 상태 조회
    [[nodiscard]] auto state()   const noexcept -> SessionState;
    [[nodiscard]] auto context() const noexcept -> const SessionContext&;
};
```

**생성자 파라미터**:
- `session_id`: 프로세스 범위 유일 ID
- `client_stream`: 클라이언트 측 AsyncStream (accept 후 평문 또는 TLS로 이미 준비됨)
  - Frontend SSL 활성화: ssl::stream 래핑된 AsyncStream
  - Frontend SSL 비활성화: tcp::socket 래핑된 AsyncStream
- `server_endpoint`: 업스트림 MySQL 서버 엔드포인트
- `backend_ssl_ctx`: Backend TLS 컨텍스트 포인터
  - Backend SSL 활성화: ssl::context* (유효한 포인터)
  - Backend SSL 비활성화: nullptr
  - Session::run()에서 backend_ssl_ctx 판단 후 AsyncStream 생성
- `policy`, `logger`, `stats`: shared 소유권 (shared_ptr)

**주요 동작**:
1. Frontend TLS 핸드셰이크 (필요한 경우):
   - `co_await client_stream_.async_handshake(ssl::stream_base::server)`
   - 평문 모드: no-op (즉시 성공)

2. Backend 연결 및 TLS:
   - TCP connect → backend_ssl_ctx 확인
   - Backend SSL: ssl::stream 생성 → SSLRequest → TLS 핸드셰이크 → AsyncStream 교체
   - Backend 평문: tcp::socket → AsyncStream 래핑

3. MySQL 핸드셰이크 (양쪽 AsyncStream 위):
   - `HandshakeRelay::relay_handshake(client_stream_, server_stream_, ctx)`
   - CLIENT_SSL 비트 strip (이중 TLS 방지)

**생명주기**:
1. 생성: kHandshaking 상태
2. run() 실행: 핸드셰이크 수행
3. 핸드셰이크 완료: kReady
4. 커맨드 루프: COM_QUERY 수신/처리
5. close() 호출: kClosing
6. 진행 중인 쿼리 완료: kClosed

**스레드 안전성**:
- 모든 비동기 핸들러는 strand 위에서 직렬화
- 수동 락 불필요
- enable_shared_from_this로 자기 참조 안전성 보장

---

## 의존 관계 규칙

```
✓ 단방향 의존만 허용
✓ 모듈별 책임 경계 명확
✓ proxy가 모든 모듈 의존 (통합점)

금지:
❌ common ← 다른 모듈 의존
❌ protocol ← parser/policy 의존
❌ parser ← policy 의존
❌ rule ← loader/engine 의존
❌ logger ← proxy 역의존
❌ stats ← proxy 역의존
```

---

## 참고 자료

- 프로젝트 스펙: `docs/project-spec-v3.md`
- 아키텍처 설명: `docs/architecture.md`
- UDS 프로토콜: `docs/uds-protocol.md`
