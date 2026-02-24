# dbgate C++ 인터페이스 레퍼런스

## 개요

이 문서는 `src/` 하위 모든 헤더 파일의 공개 인터페이스를 정의합니다. 각 헤더는 담당 엔지니어가 Phase 2(인터페이스 정의)에서 확정했으며, **이후 변경은 architect 승인이 필수**입니다.

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

    // 핸드셰이크 수행
    // client_sock: accept 된 클라이언트 소켓
    // server_sock: MySQL 서버에 연결된 소켓
    // ctx: [out] db_user, db_name, handshake_done 이 채워짐
    // 반환: co_awaitable, 성공 시 expected<void, ParseError>
    static auto relay_handshake(
        boost::asio::ip::tcp::socket& client_sock,
        boost::asio::ip::tcp::socket& server_sock,
        SessionContext&               ctx
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

### policy/policy_engine.hpp

파싱된 쿼리와 세션 컨텍스트를 받아 ALLOW/BLOCK/LOG 판정을 내립니다.

#### PolicyAction

정책 평가 결과 액션입니다.

```cpp
enum class PolicyAction : std::uint8_t {
    kAllow = 0,  // 명시적 allow 규칙 일치 시만
    kBlock = 1,  // default deny 또는 명시적 block
    kLog   = 2,  // 허용 + 감사 로그
};
```

#### PolicyResult

정책 평가 결과입니다.

```cpp
struct PolicyResult {
    PolicyAction action{PolicyAction::kBlock};  // 기본값 BLOCK
    std::string  matched_rule{};                // 규칙 ID
    std::string  reason{};                      // 판정 이유
};
```

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

    // 정책 평가
    // 평가 순서 (반드시 준수):
    //   1. SQL 구문 차단 (block_statements)
    //   2. SQL 패턴 차단 (block_patterns / injection)
    //   3. 사용자/IP 접근 제어
    //   4. 테이블 접근 제어
    //   5. 시간대 제한
    //   6. 프로시저 제어
    //   7. 명시적 allow → kAllow
    //   8. 일치 없음 → kBlock (default deny)
    [[nodiscard]] PolicyResult evaluate(
        const ParsedQuery&     query,
        const SessionContext&  session) const;

    // 파서 오류 시 호출
    // 반드시 PolicyAction::kBlock을 반환 (fail-close)
    [[nodiscard]] PolicyResult evaluate_error(
        const ParseError&     error,
        const SessionContext& session) const noexcept;

    // Hot Reload: 새 정책으로 원자적 교체
    // 진행 중인 evaluate()는 이전 config로 완료
    // 새로운 evaluate()는 new_config 사용
    void reload(std::shared_ptr<PolicyConfig> new_config);
};
```

**Fail-Close 보장**:
- 파서 오류 → evaluate_error() → kBlock
- 정책 일치 없음 → kBlock (default deny)
- 엔진 오류 → kBlock
- kAllow는 명시적 allow 규칙 필요

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
    UdsServer(const std::filesystem::path&    socket_path,
              std::shared_ptr<StatsCollector> stats,
              asio::io_context&               ioc);

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
  - 성공: `{"ok": true, "payload": {...StatsSnapshot...}}`
  - 실패: `{"ok": false, "error": "<msg>"}`

**지원 커맨드**:
- `"stats"`: StatsSnapshot 반환
- `"sessions"`: 활성 세션 목록 (Phase 3)
- `"policy_reload"`: 정책 리로드 트리거 (Phase 3)

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
};
```

**주의**: 모든 값은 config 파일에서 로드 (하드코딩 금지)

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
    Session(std::uint64_t                    session_id,
            boost::asio::ip::tcp::socket     client_socket,
            boost::asio::ip::tcp::endpoint   server_endpoint,
            std::shared_ptr<PolicyEngine>    policy,
            std::shared_ptr<StructuredLogger> logger,
            std::shared_ptr<StatsCollector>  stats);

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
