# dbgate — DB Access Control Proxy (프로젝트 설계서 v3)

> 이 문서는 Claude Code에서 새 대화를 시작할 때 컨텍스트로 제공하기 위한 프로젝트 전체 설계서입니다.

---

## 1. 프로젝트 개요

### 컨셉

- DB 접근제어 솔루션(예: DBSAFER)의 **미니 버전**
- DB 클라이언트와 MySQL 서버 사이에 위치하는 **투명 프록시**
- SQL을 파싱하여 정책 기반으로 **차단/허용/로깅** 수행
- **C++로 핵심 엔진**, **Go로 운영 도구** 구현

---

## 2. 아키텍처 개요

### 전체 배포 구조

```
                        ┌─ dbgate instance 1 ──→ MySQL Primary
[DB Client] → [HAProxy] ├─ dbgate instance 2 ──→ MySQL Primary
                        └─ dbgate instance 3 ──→ MySQL Replica
                         │
                    Health Check (/health)
```

### 각 dbgate 인스턴스 내부 구조

```
┌──────────────────────────────────────────────────┐
│  [Client Socket]                                  │
│       │                                           │
│       ▼                                           │
│  Boost.Asio (epoll 기반 비동기 I/O, C++20 코루틴)   │
│       │                                           │
│       ▼                                           │
│  MySQL Protocol Handler (핸드셰이크 패스스루)       │
│       │                                           │
│       ▼                                           │
│  SQL Parser & Policy Engine                       │
│   ├─ SQL 구문 분류 (SELECT/INSERT/DROP...)         │
│   ├─ 테이블별 접근 제어                              │
│   ├─ 사용자/IP별 권한                               │
│   ├─ 프로시저 탐지 (CALL, CREATE PROCEDURE)         │
│   ├─ SQL Injection 패턴 탐지                       │
│   └─ 시간대별 접근 제어                              │
│       │                                           │
│   [ALLOW]           [BLOCK]                       │
│       │                 │                         │
│       ▼                 ▼                         │
│  [MySQL Server]   Error Response to Client        │
│       │                                           │
│       ▼                                           │
│  Structured Logger (spdlog, JSON)                 │
│   ├─ 접속 로그                                     │
│   ├─ 쿼리 로그                                     │
│   └─ 차단 로그 (정책 매칭 정보 포함)                  │
└──────────────────────────────────────────────────┘

[Go CLI/Dashboard] ←── Unix Domain Socket ──→ [C++ Core]
```

### 핵심 설계 원칙

- **핸드셰이크 패스스루**: MySQL 핸드셰이크는 클라이언트-서버 간 투명 릴레이. 프록시는 핸드셰이크 완료 후 COM_QUERY 단계부터 파싱/정책 적용. auth plugin 호환성 문제 회피.
- **1:1 세션 릴레이**: 클라이언트 1개 = MySQL 연결 1개. 커넥션 풀링은 세션 상태(트랜잭션, SET 변수) 관리 복잡도가 높아 스코프 외.
- **SQL 파서 범위**: 풀 SQL 파서가 아닌 "첫 번째 키워드 기반 분류 + 정규식 패턴 매칭" 수준. yacc/flex 불필요. 한계는 Limitations 섹션에 명시.

---

## 3. 기술 스택

### C++ (핵심 데이터 패스 — 성능 크리티컬)

| 용도 | 라이브러리 | 이유 |
|------|-----------|------|
| 비동기 I/O | **Boost.Asio** (C++20 코루틴) | strand로 멀티스레드 안전, SSL 내장, co_await로 깔끔한 비동기 코드 |
| SSL/TLS | **Boost.Asio SSL** | 클라이언트↔프록시, 프록시↔MySQL 양 구간 암호화 |
| 로그 | **spdlog** | 구조화 JSON 로그, 고성능, 업계 표준 |
| 설정 | **yaml-cpp** | YAML 정책 파일 파싱 |
| 테스트 | **Google Test** | 표준 C++ 테스트 프레임워크 |
| 퍼징 | **libFuzzer** | MySQL 패킷 파서, SQL 파서 퍼징 |
| 빌드 | **CMake** + **vcpkg** | Boost 등 의존성 관리 |
| C++ 표준 | **C++20** | 코루틴(co_await) 활용 |
| 컴파일러 | **GCC 14** (기본), Clang 호환 유지 | Boost.Asio 코루틴 안정성, Linux 환경 표준 |

**C++20 코루틴 릴레이 코드 예시:**

```cpp
asio::awaitable<void> relay(tcp::socket& client, tcp::socket& server) {
    std::array<char, 8192> buf;
    while (true) {
        auto n = co_await client.async_read_some(
            asio::buffer(buf), asio::use_awaitable);
        auto query = parser_.extract_query(buf, n);
        if (query && !policy_.check(*query)) {
            co_await send_error(client, "Query blocked by policy");
            continue;
        }
        co_await async_write(server, asio::buffer(buf, n), asio::use_awaitable);
    }
}
```

### Go (컨트롤 플레인 — 운영 편의)

| 용도 | 라이브러리 |
|------|-----------|
| CLI 관리 도구 | cobra |
| HTTP 통계 서버 | net/http (표준 라이브러리) |
| 웹 대시보드 | Go 템플릿 + htmx |

**C++ ↔ Go 통신:**

- Unix Domain Socket으로 코어가 통계/세션 정보 노출
- Go 도구가 소켓 연결하여 조회/명령

### 인프라

| 용도 | 도구 |
|------|------|
| 로드밸런서 | HAProxy |
| 컨테이너 | Docker Compose |
| DB | MySQL (테스트용) |
| CI | GitHub Actions |

---

## 4. 개발 환경

### devcontainer (필수)

Mac(호스트) → VSCode + Claude Code → devcontainer(Ubuntu 24.04) 안에서 개발.
모든 빌드, 테스트, 실행이 컨테이너 안에서 일어남. epoll, inotify, UDS 전부 네이티브 Linux 동작.

```
.devcontainer/
├── devcontainer.json
└── Dockerfile        # Ubuntu 24.04 + g++-14 + cmake + vcpkg + go 1.22 + mysql-client
```

devcontainer.json에서 docker-compose.yaml의 MySQL 컨테이너를 함께 기동하여, 열자마자 개발+테스트 환경 준비.

---

## 5. 프로젝트 디렉토리 구조

```
dbgate/
├── .devcontainer/                 # 개발 환경
│   ├── devcontainer.json
│   └── Dockerfile
├── .github/
│   ├── pull_request_template.md   # PR 템플릿 (@codex review 포함)
│   └── workflows/
│       ├── ci.yml                 # C++ 빌드 + 테스트 + 정적/동적 분석
│       ├── go.yml                 # Go 린트 + 테스트
│       └── integration.yml        # 통합 테스트
├── src/                           # C++ 소스 (핵심 엔진)
│   ├── main.cpp
│   ├── proxy/
│   │   ├── proxy_server.hpp/cpp   # Boost.Asio TCP 프록시 서버
│   │   └── session.hpp/cpp        # 클라이언트-서버 세션 관리 (1:1 릴레이)
│   ├── protocol/
│   │   ├── mysql_packet.hpp/cpp   # MySQL 패킷 구조/파싱
│   │   ├── handshake.hpp/cpp      # MySQL 핸드셰이크 패스스루
│   │   └── command.hpp/cpp        # COM_QUERY 등 커맨드 처리
│   ├── parser/
│   │   ├── sql_parser.hpp/cpp     # SQL 구문 분류 (키워드 기반 + 정규식)
│   │   ├── procedure_detector.hpp/cpp
│   │   └── injection_detector.hpp/cpp
│   ├── policy/
│   │   ├── policy_engine.hpp/cpp  # 정책 매칭/판정 엔진
│   │   ├── policy_loader.hpp/cpp  # YAML 정책 로딩/Hot Reload
│   │   └── rule.hpp               # 정책 규칙 정의
│   ├── logger/
│   │   ├── structured_logger.hpp/cpp
│   │   └── log_types.hpp
│   ├── stats/
│   │   ├── stats_collector.hpp/cpp
│   │   └── uds_server.hpp/cpp     # Unix Domain Socket 서버 (Go 연동)
│   └── health/
│       └── health_check.hpp/cpp
├── tools/                         # Go 소스 (컨트롤 플레인)
│   ├── cmd/
│   │   └── dbgate-cli/
│   │       └── main.go
│   ├── internal/                  # Go 내부 패키지 (Go 컨벤션)
│   │   ├── client/                # UDS 클라이언트
│   │   └── dashboard/             # 웹 대시보드 핸들러
│   └── go.mod
├── tests/                         # C++ 단위 테스트
│   ├── test_mysql_packet.cpp
│   ├── test_sql_parser.cpp
│   ├── test_policy_engine.cpp
│   └── test_injection_detector.cpp
├── tests/fuzz/                    # Fuzzing 테스트
│   ├── fuzz_mysql_packet.cpp
│   ├── fuzz_sql_parser.cpp
│   └── fuzz_policy_engine.cpp
├── tests/integration/             # 통합 테스트 (docker-compose 기반)
│   └── test_scenarios.sh
├── benchmarks/                    # 벤치마크
│   ├── run_benchmark.sh           # sysbench 기반, 직접연결 vs 프록시
│   └── results/
├── config/
│   └── policy.yaml
├── docs/
│   ├── adr/                       # Architecture Decision Records
│   │   ├── 001-boost-asio-over-raw-epoll.md
│   │   ├── 002-handshake-passthrough.md
│   │   ├── 003-gcc-over-clang.md
│   │   ├── 004-yaml-policy-format.md
│   │   ├── 005-cpp-go-language-split.md
│   │   └── 006-sql-parser-scope.md
│   └── threat-model.md            # 위협 모델링 문서
├── deploy/
│   ├── Dockerfile
│   ├── Dockerfile.tools
│   └── haproxy.cfg
├── docker-compose.yaml
├── CMakeLists.txt
├── CMakePresets.json              # default/debug/asan/tsan 프리셋
├── .clang-tidy                    # C++ 정적 분석 설정
├── .clang-format                  # C++ 코드 포맷팅
├── .golangci.yml                  # Go 린터 설정
├── .gitignore
├── AGENTS.md                      # Codex 리뷰 가이드라인
├── CLAUDE.md                      # Claude Code 프로젝트 지침
├── LICENSE                        # MIT License
└── README.md
```

---

## 6. 기능 상세 (우선순위별)

### 6.1 핵심 (Core) — 반드시 구현

#### 프록시 엔진

- epoll 기반(Boost.Asio) 비동기 TCP 프록시
- 1:1 세션 릴레이 (커넥션 풀링은 스코프 외)
- Graceful Shutdown (기존 세션 완료 후 종료)
- Health Check 엔드포인트 (/health)

#### 프로토콜 파싱

- MySQL 핸드셰이크 패스스루 (투명 릴레이)
- SQL 쿼리 추출 (COM_QUERY 패킷 파싱)
- 프로시저 호출 탐지 (CALL, CREATE/ALTER PROCEDURE)

#### 정책 엔진

- YAML 기반 정책 설정 파일
- SQL 구문별 차단 (DROP, TRUNCATE, DELETE 등)
- 테이블별 접근 제어 (특정 테이블 SELECT만 허용 등)
- 사용자/IP별 권한 분리
- 프로시저 화이트리스트/블랙리스트
- 시간대별 접근 제어 (업무시간 외 차단)

#### 로깅

- 접속 로그 (누가, 언제, 어디서)
- 쿼리 로그 (어떤 SQL을, 어떤 테이블에)
- 차단 로그 (왜 차단됐는지, 정책 매칭 정보)
- JSON 구조화 로그 (ELK 등 연동 가능)

#### 에러 핸들링

- MySQL 서버 연결 실패 시: 클라이언트에 MySQL Error Packet 반환 + 로그 기록
- Malformed 패킷 수신 시: 세션 종료 + 상세 로그 (패킷 덤프)
- 정책 엔진 오류 시: Fail-close (차단) 정책 + 알림 로그
- 프록시 과부하 시: 새 연결 거부 + health check unhealthy 전환

### 6.2 중요 (Important) — 차별화

#### 보안 강화

- SQL Injection 패턴 탐지 (`' OR 1=1 --` 류)
- 동적 SQL 우회 탐지 (PREPARE/EXECUTE로 DROP 숨기기)
- 대량 데이터 유출 감지 (SELECT 결과 행 수 임계값 초과 시 알림)
- 비인가 스키마 조회 차단 (INFORMATION_SCHEMA 접근 통제)
- SSL/TLS 지원 (클라이언트↔프록시, 프록시↔MySQL 양 구간)

#### 운영

- 정책 Hot Reload (재시작 없이 설정 변경 반영 — SIGHUP or inotify)
- 실시간 통계 (초당 쿼리 수, 차단율, 활성 세션 수)
- CLI 관리 도구 (현재 세션 조회, 강제 세션 종료, 정책 리로드)
- 웹 대시보드 (실시간 QPS 차트, 차단 쿼리 목록, 활성 세션)
- 멀티 인스턴스 지원 (Docker Compose + HAProxy)

#### 품질

- 벤치마크 (sysbench: 직접연결 vs 프록시 레이턴시/TPS 비교)
- Fuzzing (libFuzzer: MySQL 패킷 파서, SQL 파서)

### 6.3 선택 (Nice-to-have) — 시간 여유 시

- Prepared Statement 처리 (COM_STMT_PREPARE / EXECUTE)
- Connection Pooling
- PostgreSQL 프로토콜 지원
- 쿼리 응답 결과 로깅 (몇 행 반환됐는지)
- 세션 리플레이 (특정 세션의 전체 쿼리 흐름 재현)
- 민감 데이터 마스킹 (주민번호, 카드번호 컬럼 자동 마스킹)
- gRPC 기반 정책 서버 연동
- Prometheus 메트릭 Export
- eBPF 연동 (커넥션 메타데이터: UID, PID, 프로세스명 캡처)

---

## 7. 정책 파일 예시 (policy.yaml)

```yaml
# dbgate 정책 설정
global:
  log_level: info
  log_format: json
  max_connections: 1000
  connection_timeout: 30s

# 사용자/IP별 접근 제어
access_control:
  - user: "admin"
    source_ip: "10.0.0.0/8"
    allowed_tables: ["*"]
    allowed_operations: ["*"]
    time_restriction: null

  - user: "readonly_user"
    source_ip: "192.168.1.0/24"
    allowed_tables: ["users", "orders", "products"]
    allowed_operations: ["SELECT"]
    time_restriction:
      allow: "09:00-18:00"
      timezone: "Asia/Seoul"

  - user: "app_service"
    source_ip: "172.16.0.0/12"
    allowed_tables: ["users", "orders"]
    allowed_operations: ["SELECT", "INSERT", "UPDATE"]
    blocked_operations: ["DROP", "TRUNCATE", "ALTER"]

# SQL 구문 차단 규칙
sql_rules:
  block_statements:
    - DROP
    - TRUNCATE
    - "DELETE FROM * WHERE 1=1"

  block_patterns:
    - "UNION\\s+SELECT"
    - "'\\s*OR\\s+'.*'\\s*=\\s*'"
    - "SLEEP\\s*\\("
    - "BENCHMARK\\s*\\("

# 프로시저 제어
procedure_control:
  mode: "whitelist"
  whitelist:
    - "sp_get_user_info"
    - "sp_update_order_status"
  block_dynamic_sql: true
  block_create_alter: true

# 데이터 유출 방지
data_protection:
  max_result_rows: 10000
  block_schema_access: true
  sensitive_columns:
    - pattern: "ssn|social_security"
    - pattern: "card_number|credit_card"

# 알림 설정
alerts:
  on_block: true
  on_high_volume_query: true
  threshold_qps: 500
```

---

## 8. 코드 품질 전략

### C++ 정적 분석

- **clang-tidy**: `.clang-tidy` 설정 파일로 빌드 시 자동 분석
- **cppcheck**: clang-tidy와 병행하여 커버리지 확보

### C++ 동적 분석

- **AddressSanitizer (ASan)**: buffer overflow, use-after-free, double-free, 메모리 누수
- **ThreadSanitizer (TSan)**: data race 탐지. 멀티스레드 + Boost.Asio strand 사용 프로젝트에서 필수
- **libFuzzer**: MySQL 패킷 파서, SQL 파서 퍼징

### CMake 프리셋 (CMakePresets.json)

```
default    — Release, 최적화
debug      — Debug, 디버그 심볼
asan       — Debug + AddressSanitizer + LeakSanitizer
tsan       — Debug + ThreadSanitizer
```

### C++ 코드 포맷팅

- **clang-format**: `.clang-format` 설정 파일로 자동 포맷팅. 세부 컨벤션은 구현하면서 확정.

### Go 코드 품질

- **golangci-lint**: `.golangci.yml`에 errcheck, staticcheck, gosec, gocritic 활성화
- **go race detector**: `go test -race`

### CI 파이프라인 구조

```
CI Pipeline
├── C++
│   ├── 빌드 (GCC 14, CMake)
│   ├── 단위 테스트 (Google Test)
│   ├── clang-tidy + cppcheck (정적)
│   ├── ASan + TSan 빌드 + 테스트 (동적)
│   └── libFuzzer (파서 퍼징)
├── Go
│   ├── golangci-lint (정적, gosec 포함)
│   └── go test -race (동적)
├── Integration Test
│   └── docker-compose + 시나리오 테스트
└── Codex Review
    └── Codex Cloud 자동 리뷰 (AGENTS.md 기반)
```

---

## 9. 프로젝트 관리

### 도구별 역할 분리

| 도구 | 역할 |
|------|------|
| **Linear** | 프로젝트/마일스톤/이슈 관리, 진행상황 추적 |
| **이 대화창 (Claude.ai)** | Architect 역할 — 설계 논의, Linear 이슈 생성/관리, 아키텍처 리뷰 |
| **Claude Code** | 실제 구현 — CLAUDE.md 기반 코딩 |
| **GitHub** | 코드 저장소, PR, CI, Codex 코드 리뷰 |

### Linear 구조

```
Project: dbgate
├── Milestone: Phase 1 — 프로젝트 세팅
├── Milestone: Phase 2 — 인터페이스 정의
├── Milestone: Phase 3 — 핵심 모듈 병렬 개발
├── Milestone: Phase 4 — 통합 + 운영 도구
├── Milestone: Phase 5 — 품질 강화
└── Milestone: Phase 6 — 대시보드 + 마무리

Labels: Network, Security, QA, DevOps, Go
```

### Linear ↔ GitHub 연동

- 브랜치명/PR 제목에 이슈 ID(DON-XX) 포함 → 자동 연결
- PR 머지 → Linear 이슈 자동 Done

### Codex 코드 리뷰

- Codex Cloud로 GitHub PR 자동 리뷰
- `AGENTS.md`에 리뷰 가이드라인 정의 (한글 작성, 보안/성능/품질 체크리스트)
- PR 템플릿에 `@codex review` 포함

### ADR 작성 시점

ADR은 해당 설계 결정이 이루어지는 Phase에서 바로 작성한다.

| ADR | 작성 시점 |
|-----|----------|
| 001-boost-asio-over-raw-epoll | Phase 1 |
| 003-gcc-over-clang | Phase 1 |
| 002-handshake-passthrough | Phase 2 |
| 005-cpp-go-language-split | Phase 2 |
| 006-sql-parser-scope | Phase 3 |
| 004-yaml-policy-format | Phase 3 |

---

## 10. 개발 전략

### 10.1 CLAUDE.md — 프로젝트 지침

Claude Code가 프로젝트 루트의 `CLAUDE.md`를 자동으로 읽음. 코딩 컨벤션, 아키텍처 규칙, 금지사항을 명시.

```markdown
# CLAUDE.md

## 프로젝트 개요
DB 접근제어 프록시. C++ 코어 + Go 운영도구.

## 아키텍처 규칙
- C++ 코어는 src/ 하위, Go 도구는 tools/ 하위
- C++ ↔ Go 통신은 반드시 Unix Domain Socket
- 새 파일 추가 시 CMakeLists.txt 업데이트 필수

## C++ 코딩 규칙
- C++20, GCC 14 기준
- 비동기는 반드시 Boost.Asio co_await 사용 (raw thread 금지)
- 메모리: shared_ptr/unique_ptr 사용, raw new/delete 금지
- 에러: 예외 대신 expected/error_code 패턴
- 로깅: spdlog 사용, fmt::format 스타일
- 네이밍: snake_case (함수/변수), PascalCase (클래스/구조체)

## Go 코딩 규칙
- 표준 Go 컨벤션, golangci-lint 통과 필수
- 에러는 반드시 처리 (_ 무시 금지)

## 테스트
- 모든 public 함수에 단위 테스트
- C++ 테스트: cmake --build build --target test
- Go 테스트: cd tools && go test -race ./...

## 브랜치 규칙
- main은 항상 빌드 + 테스트 통과 상태
- 브랜치명: feat/모듈명, fix/이슈명
- 커밋 메시지에 Linear 이슈 ID (DON-XX) 포함
- 머지 전 반드시: clang-tidy 통과, ASan/TSan 클린, 테스트 통과

## 절대 하지 말 것
- raw epoll 직접 사용
- 전역 변수
- using namespace std;
- 하드코딩된 포트/경로 (config에서 읽을 것)
```

### 10.2 인터페이스 선행 정의

모든 모듈의 인터페이스(헤더)를 먼저 확정한 후 구현에 들어간다.

```cpp
// src/protocol/mysql_packet.hpp
class MysqlPacket {
public:
    static std::expected<MysqlPacket, ParseError> parse(std::span<const uint8_t> data);
    uint8_t sequence_id() const;
    std::span<const uint8_t> payload() const;
    PacketType type() const;
};

// src/parser/sql_parser.hpp
struct ParsedQuery {
    SqlCommand command;        // SELECT, INSERT, DROP...
    std::vector<std::string> tables;
    std::string raw_sql;
};

class SqlParser {
public:
    std::expected<ParsedQuery, ParseError> parse(std::string_view sql);
};

// src/policy/policy_engine.hpp
enum class PolicyAction { Allow, Block, Log };

struct PolicyResult {
    PolicyAction action;
    std::string matched_rule;
    std::string reason;
};

class PolicyEngine {
public:
    bool load(const std::filesystem::path& config_path);
    bool reload();
    PolicyResult evaluate(const ParsedQuery& query, const SessionContext& ctx);
};
```

### 10.3 브랜치 전략

```
main (항상 빌드 가능 상태 유지)
│
├── feat/project-setup          ← Phase 1
│
├── feat/interfaces             ← Phase 2 (병렬 개발 전 먼저 머지)
│
│   ↓ 여기서부터 병렬 가능
├── feat/mysql-protocol
├── feat/sql-parser
├── feat/policy-engine
├── feat/logger
│
│   ↓ 위 4개 머지 후
├── feat/proxy-server           ← 모듈 통합
├── feat/go-cli
├── feat/ci-pipeline
│
│   ↓ 통합 후
├── feat/ssl-tls
├── feat/benchmark
├── feat/web-dashboard
├── feat/fuzz-tests
├── feat/unit-tests
├── feat/integration-tests
│
└── feat/docs-demo
```

---

## 11. 구현 일정

> 하루 8시간 기준. 주말 제외.

### Phase 1: 프로젝트 세팅 (1일 — 2/24)

- devcontainer 구성 (Ubuntu 24.04 + g++-14 + cmake + vcpkg + go)
- CMakeLists.txt + CMakePresets.json + vcpkg.json + .clang-tidy + .clang-format
- 디렉토리 구조 + CLAUDE.md + AGENTS.md + PR 템플릿 + .gitignore + LICENSE(MIT) + policy.yaml
- ADR: 001-boost-asio-over-raw-epoll, 003-gcc-over-clang
- **검증**: devcontainer에서 빈 프로젝트 빌드 성공

### Phase 2: 인터페이스 정의 (1일 — 2/25)

- 모든 모듈의 헤더 파일 작성 (구현 없이 인터페이스만)
- 타입 정의, 모듈 간 의존성 확인
- ADR: 002-handshake-passthrough, 005-cpp-go-language-split
- **검증**: 빈 구현체로 빌드 통과

### Phase 3: 핵심 모듈 병렬 개발 (4일 — 2/26 ~ 3/3)

| 브랜치 | 내용 |
|--------|------|
| feat/mysql-protocol | MySQL 패킷 파싱, 핸드셰이크 패스스루 |
| feat/sql-parser | SQL 구문 분류, Injection 탐지, 프로시저 탐지 + ADR 006 |
| feat/policy-engine | YAML 로딩, 룰 매칭, ALLOW/BLOCK 판정 + ADR 004 |
| feat/logger | spdlog 기반 구조화 로깅 |

- 각 모듈은 독립 테스트 포함
- **검증**: 모듈별 단위 테스트 전체 통과

### Phase 4: 통합 + 운영 도구 (3일 — 3/4 ~ 3/6)

| 브랜치 | 내용 |
|--------|------|
| feat/proxy-server | Boost.Asio 프록시 서버, 모듈 조립, 양방향 릴레이 |
| feat/go-cli | CLI 도구 (sessions, stats, policy reload) |
| feat/ci-pipeline | GitHub Actions + Codex 자동 리뷰 |

- 정책 Hot Reload (SIGHUP / inotify)
- 실시간 통계 수집 + UDS 서버
- **검증**: mysql CLI로 프록시 경유 접속, DROP TABLE 차단 확인

### Phase 5: 품질 강화 (3일 — 3/9 ~ 3/11)

| 브랜치 | 내용 |
|--------|------|
| feat/ssl-tls | 클라이언트↔프록시, 프록시↔MySQL SSL |
| feat/benchmark | sysbench 벤치마크, 결과 문서화 |
| feat/fuzz-tests | libFuzzer 퍼징 (패킷 파서, SQL 파서) |
| feat/unit-tests + feat/integration-tests | 추가 테스트 |

- **검증**: ASan/TSan 클린, 퍼징 크래시 제로

### Phase 6: 대시보드 + 마무리 (3일 — 3/12 ~ 3/14)

| 브랜치 | 내용 |
|--------|------|
| feat/web-dashboard | 실시간 QPS 차트, 차단 쿼리 목록, 활성 세션 |
| feat/docker-deploy | Dockerfile + docker-compose (dbgate x3 + HAProxy + MySQL) |
| feat/docs-demo | README, Threat Model, 데모 시나리오 |

- **검증**: `docker-compose up`으로 전체 환경 기동, 데모 시연 가능

---

## 12. 데모 시나리오

### 기본 시나리오

```bash
# 1. 전체 환경 기동
docker-compose up -d

# 2. 프록시 경유 MySQL 접속
mysql -h localhost -P 13306 -u app_service -p

# 3. 정상 쿼리 → 통과
SELECT * FROM users WHERE id = 1;

# 4. 차단 대상 쿼리 → 블록
DROP TABLE users;
# → ERROR: "Query blocked by policy: DROP statement not allowed"

# 5. SQL Injection 시도 → 탐지/차단
SELECT * FROM users WHERE name = '' OR 1=1 --';
# → ERROR: "Query blocked: SQL injection pattern detected"

# 6. 업무시간 외 접근 → 차단 (readonly_user)
# → ERROR: "Access denied: outside allowed time window"

# 7. CLI로 실시간 모니터링
dbgate-cli sessions
dbgate-cli stats
dbgate-cli policy reload

# 8. 웹 대시보드
# → http://localhost:8080
```

### 벤치마크 시나리오

```bash
# 직접 연결
sysbench oltp_read_only --mysql-host=localhost --mysql-port=3306 run

# 프록시 경유
sysbench oltp_read_only --mysql-host=localhost --mysql-port=13306 run
```

---

## 13. 핵심 설계 결정 요약

| 결정 | 선택 | 이유 |
|------|------|------|
| 언어 분리 | C++ 데이터패스 + Go 컨트롤플레인 | 성능 vs 편의 적재적소 |
| Async I/O | Boost.Asio (raw epoll 대신) | 핵심 로직 집중, strand로 스레드 안전, SSL 내장 |
| 컴파일러 | GCC 14 (Clang 호환 유지) | 코루틴 안정성, Linux 표준 |
| C++ 표준 | C++20 | co_await 코루틴으로 비동기 코드 간결화 |
| 핸드셰이크 | 패스스루 (투명 릴레이) | auth plugin 호환성, 구현 복잡도 회피 |
| SQL 파서 | 키워드 분류 + 정규식 | 풀 파서 불필요, 한계 명시 |
| 세션 모델 | 1:1 릴레이 | 커넥션 풀링의 세션 상태 관리 복잡도 회피 |
| 로깅 | spdlog | 업계 표준, JSON 구조화 로그 |
| 설정 | yaml-cpp | 사람이 읽기 쉬운 정책 파일 |
| 개발환경 | devcontainer (Ubuntu 24.04) | Linux 네이티브, 재현 가능 |
| 배포 | Docker Compose + HAProxy | 멀티 인스턴스 운영 |
| IPC | Unix Domain Socket | C++ ↔ Go 간 저오버헤드 통신 |
| 코드 품질 | ASan/TSan/libFuzzer + clang-tidy + golangci-lint | 메모리/스레드 안전성 증명 |
| 프로젝트 관리 | Linear + GitHub | 이슈 추적 + PR 자동 연동 |
| 코드 리뷰 | Codex Cloud + AGENTS.md | 자동 한글 리뷰, 보안/성능 체크리스트 |

---

## 14. Limitations & Future Work

### 현재 한계

- **SQL 파서가 풀 파서가 아님**: 키워드 기반 분류 + 정규식이라 복잡한 서브쿼리, 주석 내 SQL(`DROP/**/TABLE`), 인코딩 우회를 놓칠 수 있음
- **MySQL 프로토콜만 지원**: PostgreSQL, MariaDB 고유 기능은 미지원
- **Prepared Statement 바이너리 프로토콜 미파싱**: COM_QUERY만 파싱
- **커넥션 풀링 미지원**: 1:1 세션 릴레이만 지원
- **프로토콜 레벨 DoS 미대응**: slowloris 등 미방어
- **패턴 매칭 오탐 가능**: ORM 생성 쿼리 등에서 false positive 발생 가능

### 향후 확장 방향

- SQL 정규화 후 파싱 (주석 제거, 대소문자 통일, 공백 정규화)
- Protocol Handler 인터페이스 추상화 → PostgreSQL 플러그인
- eBPF로 커넥션 메타데이터 (UID, PID, 프로세스명) 캡처
- gRPC 기반 원격 정책 서버
- Prometheus 메트릭 Export
