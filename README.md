# dbgate

[![C++ CI](https://github.com/dongwonkwak/dbgate/actions/workflows/ci.yml/badge.svg)](https://github.com/dongwonkwak/dbgate/actions/workflows/ci.yml)
[![Go CI](https://github.com/dongwonkwak/dbgate/actions/workflows/go.yml/badge.svg)](https://github.com/dongwonkwak/dbgate/actions/workflows/go.yml)
[![Lint](https://github.com/dongwonkwak/dbgate/actions/workflows/lint.yml/badge.svg)](https://github.com/dongwonkwak/dbgate/actions/workflows/lint.yml)

MySQL 클라이언트와 서버 사이에 위치하는 **DB 접근제어 프록시**. SQL을 실시간으로 파싱하고 정책 기반으로 차단/허용/로깅을 수행한다.

높은 성능이 요구되는 데이터패스(data path)는 **C++23**(Boost.Asio, 코루틴)으로, 운영 편의성이 중요한 컨트롤플레인(control plane)은 **Go**로 구현되어 있으며, 두 컴포넌트는 Unix Domain Socket으로 통신한다.

Network DLP 개발 경험을 DB 접근제어 도메인에 적용한 포트폴리오 프로젝트. DBSAFER 같은 상용 DB 보안 솔루션의 핵심 기능(SQL 파싱, 정책 기반 차단, 감사 로그)을 C++23/Go로 직접 구현.

## 아키텍처

```
Client ──► HAProxy(:13306) ──┬── dbgate-1 ──► MySQL Primary
           stats(:8404)      ├── dbgate-2 ──► MySQL Replica
                             └── dbgate-3 ──►

각 dbgate 인스턴스:
┌──────────────────────────────────────────────┐
│  TCP Accept (Boost.Asio, epoll, 코루틴)       │
│       │                                       │
│  핸드셰이크 패스스루 (투명 인증 릴레이)         │
│       │                                       │
│  COM_QUERY ──► SQL Parser ──► Policy Engine  │
│                    │              │           │
│              InjectionDetector   ALLOW/BLOCK  │
│              ProcedureDetector      │         │
│                                  Logger       │
│                                  Stats ◄─► Go CLI/Dashboard (UDS)
└──────────────────────────────────────────────┘
```

## 주요 기능

- **SQL 구문 분류 및 차단**: SELECT, INSERT, UPDATE, DELETE, DROP, TRUNCATE 등 구문별 접근 제어
- **SQL Injection 탐지**: 정규식 패턴 기반 (Boolean-based, Time-based, UNION SELECT)
- **프로시저 제어**: whitelist/blacklist 모드, 동적 SQL(`PREPARE`/`EXECUTE`) 차단
- **사용자/IP/시간대별 접근 제어**: CIDR 기반 IP 필터, 시간대 제한, 테이블별 권한
- **스키마 접근 차단**: `information_schema`, `mysql`, `performance_schema`, `sys` 보호
- **정책 Hot Reload**: SIGHUP 시그널로 재시작 없이 정책 변경 반영
- **Fail-Close 원칙**: 파싱 실패, 정책 오류, 미분류 SQL 등 불확실한 상황에서 항상 차단
- **SSL/TLS**: Frontend(클라이언트↔프록시), Backend(프록시↔MySQL) 독립 TLS 지원
- **투명 인증 릴레이**: 핸드셰이크를 패스스루하여 auth plugin에 개입하지 않음
- **웹 대시보드**: Go + htmx 기반 실시간 모니터링 (QPS, 차단율, 세션 현황)
- **CLI 도구**: 세션 조회, 통계, 정책 리로드 등 운영 명령
- **Policy Explain / Dry-run API**: 정책 적용 시뮬레이션 및 설명 생성
- **Staged Rollout**: 정책 변경 시 monitor 모드→enforce 모드 단계적 전환
- **정책 버전 관리 및 즉시 롤백**: 정책 변경 이력 관리 및 빠른 복구

## Quick Start

### DevContainer (권장)

VS Code에서 DevContainer로 즉시 개발 환경을 구성할 수 있다.

1. VS Code에서 이 저장소를 열고 "Reopen in Container" 선택
2. C++ 빌드 및 Go 도구 체인이 자동 설정됨

### 직접 빌드

**요구사항**: GCC 14+, CMake 3.24+, Go 1.21+, vcpkg

```bash
# C++ 코어 빌드
cmake --preset default && cmake --build build/default

# 테스트 실행
cmake --build build/default --target test

# Go 도구 빌드
cd tools && go build ./...
```

### Docker 배포

```bash
cd deploy
cp .env.example .env   # 시크릿 값 반드시 수정
docker compose up --build -d

# HAProxy 경유 접속
mysql -h 127.0.0.1 -P 13306 -u dbgate -p
```

## 데모

### 1. DROP TABLE 차단

```bash
$ mysql -h 127.0.0.1 -P 13306 -u app_service -p -e "DROP TABLE users;"
Enter password:
ERROR 1105 (HY000): Access denied by policy: [SQL Rule Violation] Blocked statement: DROP
Connection closed
```

정책에서 DROP 문을 차단하면, 프록시가 즉시 클라이언트에게 오류를 반환한다.

### 2. SQL Injection 탐지

```bash
$ mysql -h 127.0.0.1 -P 13306 -u app_service -p \
  -e "SELECT * FROM users WHERE id=1 UNION SELECT * FROM orders;"
Enter password:
ERROR 1105 (HY000): Access denied by policy: [InjectionDetector] UNION-based SQLi detected
Connection closed
```

UNION SELECT 패턴을 정규식으로 탐지하여 차단한다.

### 3. 정상 쿼리는 통과

```bash
$ mysql -h 127.0.0.1 -P 13306 -u app_service -p -e "SELECT * FROM users LIMIT 5;"
Enter password:
+----+-------+---------+
| id | name  | email   |
+----+-------+---------+
|  1 | Alice | a@...   |
|  2 | Bob   | b@...   |
| ...
+----+-------+---------+
```

정책에 위배되지 않는 SELECT 쿼리는 정상 통과한다.

### 4. 보안 정책 실행 흐름

```
DROP TABLE 차단:

    Client ──"DROP TABLE users;"──> dbgate Parser ──> Policy ──> ❌ DENY
                                                                   │
                                                          ERROR 1105
                                                                   │
                                                        ◄──────────┘

SQL Injection 탐지:

    Attacker ──"SELECT...UNION SELECT..."──> Injection Detector ──> ❌ BLOCK
                                              (Regex Pattern Match)
                                                      │
                                            Connection Terminated

정상 쿼리 통과:

    User ──"SELECT * FROM users LIMIT 5;"──> Parser ──> Policy ──> ✓ ALLOW
                                                             │
                                                        MySQL Server
                                                             │
                                                         Result Set
                                                             │
                                                    ◄────────┘
```

전체 데모 시나리오는 [docs/demo-scenarios.md](docs/demo-scenarios.md)를 참조한다.

## 설정

### 환경변수

| 환경변수 | 기본값 | 설명 |
|----------|--------|------|
| `PROXY_LISTEN_ADDR` | `0.0.0.0` | 프록시 리슨 주소 |
| `PROXY_LISTEN_PORT` | `13306` | 프록시 리슨 포트 |
| `MYSQL_HOST` | `127.0.0.1` | 업스트림 MySQL 호스트 |
| `MYSQL_PORT` | `3306` | 업스트림 MySQL 포트 |
| `POLICY_PATH` | `config/policy.yaml` | 정책 파일 경로 |
| `LOG_LEVEL` | `info` | 로그 레벨 (trace/debug/info/warn/error) |
| `LOG_PATH` | `/tmp/dbgate.log` | 로그 파일 경로 |
| `UDS_SOCKET_PATH` | `/tmp/dbgate.sock` | Go 운영도구 UDS 소켓 경로 |
| `HEALTH_CHECK_PORT` | `8080` | 헬스체크 HTTP 포트 |
| `MAX_CONNECTIONS` | `1000` | 최대 동시 연결 수 |
| `CONNECTION_TIMEOUT_SEC` | `30` | 세션 유휴 타임아웃 (초) |

### 정책 파일

정책은 YAML 형식으로 작성한다. 상세 구조는 [docs/policy-engine.md](docs/policy-engine.md)를 참조.

```yaml
# config/policy.yaml (요약)
sql_rules:
  block_statements: [DROP, TRUNCATE]
  block_patterns:
    - "UNION\\s+SELECT"
    - "SLEEP\\s*\\("

access_control:
  - user: "app_service"
    source_ip: "172.16.0.0/12"
    allowed_tables: ["users", "orders"]
    allowed_operations: ["SELECT", "INSERT", "UPDATE"]

procedure_control:
  mode: "whitelist"
  whitelist: ["sp_get_user_info"]
  block_dynamic_sql: true
```

### 시그널 제어

| 시그널 | 동작 |
|--------|------|
| `SIGTERM` | Graceful Shutdown (활성 세션 완료 후 종료) |
| `SIGINT` | Graceful Shutdown |
| `SIGHUP` | 정책 파일 Hot Reload |

## 벤치마크

sysbench를 사용하여 직접 연결 대비 프록시 경유 성능을 측정한다.

```bash
./benchmarks/run_benchmark.sh --threads 4 --time 30
```

### 실측 결과

**환경**: MySQL 8.0 (Docker, tmpfs), 4 스레드, 30초 실행, 10K 행 × 4 테이블

| 워크로드 | 모드 | TPS | QPS | Avg(ms) | P95(ms) | P95 오버헤드 |
|---------|------|-----|-----|---------|---------|-------------|
| **oltp_read_only** | 직접연결 | 2,561 | 40,973 | 1.56 | 1.93 | - |
| | 프록시 | 442 | 7,071 | 9.05 | 9.73 | +404% |
| **oltp_read_write** | 직접연결 | 1,686 | 33,722 | 2.37 | 2.61 | - |
| | 프록시 | 392 | 7,837 | 10.21 | 11.04 | +322% |

> **참고**: 현재 프록시는 쿼리별 SQL 파싱 + 정책 평가를 동기적으로 수행하여 오버헤드가 큼. 성능 최적화는 향후 과제.

결과는 `benchmarks/results/latest.json`에 저장된다. 재현 방법과 상세 옵션은 [docs/demo-scenarios.md](docs/demo-scenarios.md#시나리오-6-벤치마크-실행)를 참조.

## 보안

dbgate는 **fail-close** 원칙을 따른다. 파싱 실패, 정책 오류, 미분류 SQL 등 불확실한 상황에서는 항상 차단한다.

**알려진 한계**:
- 주석 분할 우회 (`UN/**/ION SEL/**/ECT`): 일부 미탐
- 인코딩 우회 (URL 인코딩, Hex 리터럴): 미탐
- `COM_STMT_PREPARE` 바이너리 프로토콜: 미파싱
- 변수 간접 참조 (`SET @q = '...'; PREPARE s FROM @q`): 미탐

상세 위협 분석과 완화 상태는 [docs/threat-model.md](docs/threat-model.md)를 참조한다.

## 프로젝트 구조

```
dbgate/
├── src/                    # C++ 코어
│   ├── common/             #   공통 타입/유틸
│   ├── protocol/           #   MySQL 프로토콜 파싱
│   ├── parser/             #   SQL 파서, Injection/Procedure 탐지
│   ├── policy/             #   정책 엔진, YAML 로더
│   ├── proxy/              #   Boost.Asio 프록시 서버, 세션 관리
│   ├── health/             #   HTTP 헬스체크
│   ├── logger/             #   구조화 로거
│   ├── stats/              #   통계 수집, UDS 서버
│   └── main.cpp            #   엔트리포인트
├── tools/                  # Go 운영도구
│   ├── cmd/                #   CLI/Dashboard 엔트리포인트
│   ├── dbgate-cli/         #   CLI 도구
│   └── internal/           #   내부 패키지
├── tests/                  # C++ 단위/통합/퍼즈 테스트
├── config/                 # 정책 파일 (policy.yaml)
├── deploy/                 # Docker, HAProxy, Dockerfile
├── benchmarks/             # sysbench 벤치마크
├── docs/                   # 설계 문서, ADR, 런북
└── scripts/                # Git hooks, CI 스크립트
```

## 문서

| 문서 | 설명 |
|------|------|
| [docs/architecture.md](docs/architecture.md) | 아키텍처 설명서 |
| [docs/threat-model.md](docs/threat-model.md) | 위협 모델 |
| [docs/demo-scenarios.md](docs/demo-scenarios.md) | 데모 시나리오 |
| [docs/policy-engine.md](docs/policy-engine.md) | 정책 엔진 상세 |
| [docs/data-flow.md](docs/data-flow.md) | 데이터 흐름 |
| [docs/interface-reference.md](docs/interface-reference.md) | 인터페이스 레퍼런스 |
| [docs/uds-protocol.md](docs/uds-protocol.md) | UDS 프로토콜 |
| [docs/sequences.md](docs/sequences.md) | 시퀀스 다이어그램 |
| [docs/testing-strategy.md](docs/testing-strategy.md) | 테스트 전략 |
| [docs/runbook.md](docs/runbook.md) | 운영 런북 |
| [docs/project-spec-v3.md](docs/project-spec-v3.md) | 프로젝트 스펙 |
| [docs/adr/](docs/adr/) | Architecture Decision Records (7건) |

## Limitations & Future Work

### 현재 한계

- **SQL 파서의 경량성**: 복잡한 서브쿼리 미지원, 주석 분할/인코딩 우회 일부 미탐
- **Prepared Statement**: `COM_STMT_PREPARE` 바이너리 프로토콜 미파싱
- **세션 모델**: 1:1 릴레이만 지원 (커넥션 풀링 미지원)
- **DoS 방어**: Slowloris 같은 프로토콜 레벨 공격 미방어

### 향후 확장

- SQL 정규화 (주석 제거, 공백 정규화) 전처리 강화
- 풀 SQL 파서 (yacc/flex) 도입 검토
- eBPF로 프로세스 메타데이터 캡처 (ironpost 프로젝트와 연동 검토)
- Prometheus 메트릭 Export
- PostgreSQL 프로토콜 지원

## Contributing

기여 가이드, 커밋 컨벤션, PR 워크플로우는 [CONTRIBUTING.md](CONTRIBUTING.md)를 참조한다.
