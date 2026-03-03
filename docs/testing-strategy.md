# dbgate 테스트 전략

> 이 문서는 dbgate 프로젝트의 전체 테스트 전략을 정의한다.
> 단위 테스트, 통합 테스트, 벤치마크, 퍼징의 목적·범위·실행 방법을 기술한다.

---

## 1. 테스트 피라미드 개요

```
        ┌──────────┐
        │  E2E /   │  docker-compose + mysql CLI
        │ 통합 테스트│  tests/integration/
        ├──────────┤
        │  퍼징     │  libFuzzer + ASan
        │ (Fuzz)   │  tests/fuzz/
        ├──────────┤
        │ 벤치마크  │  sysbench 기반
        │(Benchmark)│  benchmarks/
        ├──────────┤
        │          │
        │ 단위 테스트│  Google Test
        │ (Unit)   │  tests/test_*.cpp
        │          │
        └──────────┘
```

| 계층 | 프레임워크 | 경로 | 실행 빈도 |
|------|-----------|------|----------|
| 단위 테스트 | Google Test | `tests/test_*.cpp` | 매 커밋 (CI) |
| 통합 테스트 | Bash + docker-compose | `tests/integration/` | PR 머지 시 (CI) |
| 퍼징 | libFuzzer + ASan | `tests/fuzz/` | 주기적 / PR 시 smoke |
| 벤치마크 | sysbench | `benchmarks/` | 수동 / 릴리스 전 |

---

## 2. 단위 테스트

### 2.1 목적
각 모듈의 개별 함수/클래스가 명세대로 동작하는지 검증한다.

### 2.2 대상 모듈 및 테스트 파일

| 모듈 | 테스트 파일 | 주요 검증 항목 |
|------|-----------|---------------|
| protocol | `test_mysql_packet.cpp` | 패킷 파싱, 시퀀스 번호, 경계값 |
| protocol | `test_handshake_auth.cpp` | 핸드셰이크 시퀀스, 인증 플러그인 |
| parser | `test_sql_parser.cpp` | SQL 구문 분류, 테이블 추출 |
| parser | `test_injection_detector.cpp` | SQLi 패턴 탐지, 오탐/미탐 |
| policy | `test_policy_engine.cpp` | 정책 매칭, 차단/허용 판정 |
| logger | `test_logger.cpp` | 구조화 로그 포맷, 필드 검증 |
| stats | `test_stats_collector.cpp` | 통계 수집, 카운터 정확성 |
| stats | `test_uds_server.cpp` | UDS 통신, 프로토콜 검증 |
| proxy | `test_proxy_server.cpp` | 서버 시작/종료, 연결 수락 |
| proxy | `test_proxy_pipeline.cpp` | 파이프라인 조립, 모듈 연동 |
| 공통 | `test_edge_cases.cpp` | 경계값, 엣지 케이스 |

### 2.3 실행 방법

```bash
# 기본 빌드 + 테스트
cmake --preset default && cmake --build build/default
cd build/default && ctest --output-on-failure

# ASan 빌드 + 테스트
cmake --preset asan && cmake --build build/asan
cd build/asan && ctest --output-on-failure

# TSan 빌드 + 테스트
cmake --preset tsan && cmake --build build/tsan
cd build/tsan && ctest --output-on-failure
```

### 2.4 커버리지 목표
- 각 모듈 라인 커버리지 **80% 이상** 목표
- 정책 엔진, SQL 파서, 인젝션 탐지기는 **90% 이상** 목표 (보안 크리티컬)

---

## 3. 통합 테스트

### 3.1 목적
실제 MySQL 서버와 연동하여 프록시의 End-to-End 동작을 검증한다.

### 3.2 테스트 환경

```
[mysql CLI] → [dbgate proxy :13306] → [MySQL Server :3306]
```

- docker-compose로 MySQL + dbgate 동시 기동
- `tests/integration/test_scenarios.sh`로 시나리오 실행

### 3.3 테스트 시나리오

| 시나리오 | 기대 결과 |
|---------|----------|
| 정상 SELECT 쿼리 | 릴레이 성공, 결과 반환 |
| DROP TABLE 시도 | 차단, MySQL Error Packet 반환 |
| TRUNCATE 시도 | 차단 |
| SQL Injection 패턴 | 차단 |
| 프로시저 호출 (허용) | 릴레이 성공 |
| 프로시저 호출 (차단) | 차단 |
| 시간대 외 접근 | 차단 |
| 대량 연결 | max_connections 초과 시 거부 |
| Graceful Shutdown | 기존 세션 완료 후 종료 |

### 3.4 실행 방법

```bash
# docker-compose 환경 기동 후
./tests/integration/test_scenarios.sh
```

---

## 4. 퍼징 테스트 (libFuzzer)

> 관련 이슈: DON-33

### 4.1 목적
임의의 입력을 파서/엔진에 주입하여 크래시, 무한루프, 메모리 오류를 사전에 발견한다.

### 4.2 퍼징 대상

| 퍼저 | 파일 | 대상 함수 |
|------|------|----------|
| MySQL 패킷 퍼저 | `tests/fuzz/fuzz_mysql_packet.cpp` | `MysqlPacket::parse()` |
| SQL 파서 퍼저 | `tests/fuzz/fuzz_sql_parser.cpp` | `SqlParser::parse()` |
| 정책 엔진 퍼저 | `tests/fuzz/fuzz_policy_engine.cpp` | `PolicyEngine::evaluate()` |

### 4.3 빌드 및 실행

```bash
# Clang + libFuzzer + ASan 빌드 (퍼징 전용)
# CMake 퍼징 프리셋 사용 (추후 추가)
cmake -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DFUZZING=ON -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer,address" \
      -B build/fuzz
cmake --build build/fuzz --target fuzz_mysql_packet fuzz_sql_parser fuzz_policy_engine

# 퍼저 실행 (60초, 시드 코퍼스 사용)
./build/fuzz/fuzz_mysql_packet tests/fuzz/corpus/mysql_packet/ -max_total_time=60
./build/fuzz/fuzz_sql_parser tests/fuzz/corpus/sql_parser/ -max_total_time=60
./build/fuzz/fuzz_policy_engine tests/fuzz/corpus/policy_engine/ -max_total_time=60
```

### 4.4 시드 코퍼스
- `tests/fuzz/corpus/mysql_packet/` — 정상 MySQL 패킷 바이너리 샘플
- `tests/fuzz/corpus/sql_parser/` — 정상/비정상 SQL 문자열 샘플
- `tests/fuzz/corpus/policy_engine/` — 정상 SQL + SessionContext 조합

### 4.5 CI 연동
- PR 시: 각 퍼저 **10초** smoke 실행
- 주간: 각 퍼저 **10분** 심층 실행
- 크래시 발견 시 재현 입력을 `tests/fuzz/crashes/`에 자동 저장

---

## 5. 벤치마크 (sysbench)

> 관련 이슈: DON-32

### 5.1 목적
프록시 경유 시 오버헤드를 정량 측정하여 성능 기준선을 확보한다.

### 5.2 측정 구성

```
[직접 연결]  sysbench → MySQL :3306
[프록시 경유] sysbench → dbgate :13306 → MySQL :3306
```

### 5.3 워크로드

| 워크로드 | 설명 |
|---------|------|
| `oltp_read_only` | SELECT 중심, 읽기 성능 측정 |
| `oltp_read_write` | 혼합 워크로드, 전반적 성능 |

### 5.4 측정 항목

| 항목 | 설명 |
|------|------|
| TPS | 초당 트랜잭션 수 |
| QPS | 초당 쿼리 수 |
| Avg Latency | 평균 지연시간 (ms) |
| P95 Latency | 95번째 백분위 지연시간 |
| P99 Latency | 99번째 백분위 지연시간 |
| Proxy Overhead | (프록시 - 직접) / 직접 × 100% |

### 5.5 실행 방법

```bash
# docker-compose 환경 기동 후
./benchmarks/run_benchmark.sh

# 결과 확인
cat benchmarks/results/latest.json
```

### 5.6 성능 목표
- 프록시 오버헤드 **10% 이내** (P95 레이턴시 기준)
- 단일 인스턴스 **1,000 TPS 이상** 처리 가능

---

## 6. SSL/TLS 테스트

> 관련 이슈: DON-31

### 6.1 목적
SSL/TLS 양 구간 암호화 활성화 시 정상 동작 및 보안성을 검증한다.

### 6.2 테스트 항목

| 항목 | 검증 내용 |
|------|----------|
| 정상 SSL 접속 | SSL 활성화 시 프록시 경유 쿼리 성공 |
| 인증서 검증 | 유효하지 않은 인증서 시 연결 거부 |
| TLS 버전 강제 | TLS 1.2 미만 거부 |
| 평문 호환 | SSL 비활성화 시 기존 동작 유지 |
| 성능 영향 | SSL 핸드셰이크 오버헤드 측정 |
| 인증서 교체 | 런타임 인증서 교체 시 새 연결 반영 |

---

## 7. CI 파이프라인 연동

```
CI Pipeline
├── C++
│   ├── 빌드 (GCC 14, CMake)
│   ├── 단위 테스트 (Google Test)
│   ├── clang-tidy + cppcheck (정적 분석)
│   ├── ASan + TSan 빌드 + 테스트 (동적 분석)
│   └── libFuzzer smoke (10초, PR 시)
├── Go
│   ├── golangci-lint (정적, gosec 포함)
│   └── go test -race (동적)
├── Integration Test
│   └── docker-compose + 시나리오 테스트
├── Benchmark (릴리스 전, 수동)
│   └── sysbench 직접/프록시 비교
└── Codex Review
    └── Codex Cloud 자동 리뷰 (AGENTS.md 기반)
```

---

## 8. 테스트 데이터 관리

- 정책 파일: `config/policy.yaml` (테스트용 기본 정책)
- 퍼징 코퍼스: `tests/fuzz/corpus/` (git 추적)
- 벤치마크 결과: `benchmarks/results/` (`.gitignore`에 추가, CI 아티팩트로 보관)
- 통합 테스트: docker-compose로 MySQL 자동 기동, 테스트 데이터 자동 생성

---

## 관련 문서
- [프로젝트 설계서](project-spec-v3.md) — 전체 아키텍처 및 기술 스택
- [정책 엔진](policy-engine.md) — 정책 규칙 상세
- [데이터 흐름](data-flow.md) — 패킷 처리 흐름
- [운영 매뉴얼](runbook.md) — 운영 환경 설정
