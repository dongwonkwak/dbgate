# dbgate 위협 모델 (Threat Model)

## 목적

이 문서는 dbgate의 보안 경계, 알려진 위협, 현재 완화 상태, 그리고 향후 계획을 정리한다.
소스 코드 주석(`injection_detector.hpp`, `sql_parser.hpp`, `procedure_detector.hpp`)과
[ADR-006](adr/006-sql-parser-scope.md), [아키텍처 문서](architecture.md)의 한계 섹션을 기반으로 작성되었다.

## 관련 문서

- [아키텍처 설명서](architecture.md)
- [정책 엔진](policy-engine.md)
- [ADR-006: SQL 파서 스코프](adr/006-sql-parser-scope.md)
- [데이터 흐름](data-flow.md)

---

## 보안 경계 (Trust Boundary)

```
                    신뢰 경계
                       │
  ┌────────────┐       │       ┌──────────────────────────────────┐
  │            │       │       │           dbgate                  │
  │  DB Client │───────┼──────▶│  ┌────────┐  ┌────────────────┐ │
  │ (비신뢰)   │       │       │  │ Parser │─▶│ Policy Engine  │ │
  │            │◀──────┼───────│  └────────┘  └───────┬────────┘ │
  └────────────┘       │       │                      │          │
                       │       │              ┌───────▼────────┐ │
                       │       │              │ ALLOW / BLOCK  │ │
                       │       │              └───────┬────────┘ │
                       │       │                      │          │
                       │       │              ┌───────▼────────┐ │
                       │       │              │  MySQL Server  │ │
                       │       │              │   (신뢰)       │ │
                       │       └──────────────┴────────────────┘ │
                       │
```

| 영역 | 신뢰 수준 | 설명 |
|------|----------|------|
| DB Client | **비신뢰** | 외부 애플리케이션, 개발자 도구. SQL Injection 시도 가능 |
| dbgate 프록시 | **신뢰 경계** | SQL 파싱, 정책 판정, 로깅 수행 |
| MySQL Server | **신뢰** | 업스트림 DB. dbgate가 보호하는 대상 |
| UDS (Unix Domain Socket) | **내부** | C++ 코어 ↔ Go 운영도구 통신. 파일시스템 권한으로 보호 |
| 정책 파일 (policy.yaml) | **내부** | 읽기 전용 마운트. 파일시스템 접근 제어 필요 |

---

## 위협 1: SQL Injection 우회

**공격 목표**: 정책 엔진의 `block_patterns` / `block_statements` 검사를 우회하여 위험 SQL 실행

### 1.1 주석 분할 (Comment Splitting)

| 항목 | 설명 |
|------|------|
| 공격 예시 | `UN/**/ION SEL/**/ECT`, `DROP/**/TABLE users` |
| 현재 상태 | `SqlParser`가 전처리 단계에서 `/* ... */` 주석을 제거하므로 `DROP/**/TABLE` → `DROP  TABLE`로 탐지됨. 단, `UN/**/ION SEL/**/ECT`는 `InjectionDetector` 수준에서 **탐지 불가** |
| 위험도 | **중** |
| 완화 계획 | SQL 정규화 전처리 강화 (주석 제거 후 연결된 토큰 재조합) |

> 참조: `src/parser/injection_detector.hpp:17-18`, `docs/adr/006-sql-parser-scope.md:60-63`

### 1.2 인코딩 우회 (Encoding Bypass)

| 항목 | 설명 |
|------|------|
| 공격 예시 | URL 인코딩 `%53%45%4C%45%43%54`, Hex 리터럴 `0x44524f50`, `CHAR()` 함수 |
| 현재 상태 | **탐지 불가** |
| 위험도 | **중** |
| 완화 계획 | 네트워크 레이어에서 URL 디코딩 적용, Hex 리터럴 패턴 추가 |

> 참조: `src/parser/injection_detector.hpp:21`, `docs/adr/006-sql-parser-scope.md:66-70`

### 1.3 대소문자 변형 (Case Variation)

| 항목 | 설명 |
|------|------|
| 공격 예시 | `sElEcT`, `DrOp TaBlE` |
| 현재 상태 | **완화됨** — 파서가 대문자 정규화 수행, `InjectionDetector`가 case-insensitive 매칭 사용 |
| 위험도 | **하** |

> 참조: `src/parser/injection_detector.hpp:19`

### 1.4 공백 변형 (Whitespace Variation)

| 항목 | 설명 |
|------|------|
| 공격 예시 | `DROP\tTABLE`, `SELECT\n*\nFROM users` |
| 현재 상태 | **부분 완화** — 탭·줄바꿈 치환 전처리가 부분 적용됨. 개선 여지 있음 |
| 위험도 | **하~중** |
| 완화 계획 | 공백 정규화 전처리 강화 |

> 참조: `src/parser/injection_detector.hpp:20`, `src/parser/sql_parser.hpp:15-16`

### 1.5 Multi-Statement (Piggybacking)

| 항목 | 설명 |
|------|------|
| 공격 예시 | `SELECT 1; DROP TABLE users` |
| 현재 상태 | `SqlParser`가 문자열 리터럴/주석 외부의 세미콜론 감지 시 `ParseError(kInvalidSql)` 반환 → **fail-close 차단**. 추가로 `InjectionDetector`의 피기백 패턴(`;...DROP`)으로 보완 |
| 위험도 | **하** (현재 완화됨) |

> 참조: `src/parser/sql_parser.hpp:58-62`, `docs/adr/006-sql-parser-scope.md:72-74`

---

## 위협 2: Prepared Statement 우회

**공격 목표**: `PREPARE`/`EXECUTE`를 이용하여 정책 검사를 우회, 위험 SQL을 간접 실행

### 2.1 COM_STMT_PREPARE 바이너리 프로토콜

| 항목 | 설명 |
|------|------|
| 공격 시나리오 | MySQL 바이너리 프로토콜(`COM_STMT_PREPARE`)을 사용하면 SQL이 텍스트가 아닌 바이너리로 전달 |
| 현재 상태 | **미파싱** — 바이너리 프로토콜 수준의 Prepared Statement는 파싱하지 않음 |
| 위험도 | **고** |
| 완화 계획 | `COM_STMT_PREPARE` 패킷 파싱 구현 (향후) |

> 참조: `docs/architecture.md:947-949`

### 2.2 PREPARE/EXECUTE 동적 SQL

| 항목 | 설명 |
|------|------|
| 공격 예시 | `PREPARE stmt FROM 'DROP TABLE users'; EXECUTE stmt;` |
| 현재 상태 | `ProcedureDetector`가 `is_dynamic_sql=true`로 마킹 → `procedure_control.block_dynamic_sql=true` 설정 시 **차단**. 단, 문자열 리터럴 내부의 실제 SQL 내용은 파싱하지 않음 |
| 위험도 | **중** (block_dynamic_sql=true 시 완화) |

> 참조: `src/parser/procedure_detector.hpp:10-13`

### 2.3 변수 간접 참조 (Variable Indirection)

| 항목 | 설명 |
|------|------|
| 공격 예시 | `SET @q = 'DROP TABLE users'; PREPARE s FROM @q; EXECUTE s;` |
| 현재 상태 | `@q`의 실제 값을 추적하지 못함 → **탐지 불가 (false negative)** |
| 위험도 | **고** |
| 완화 계획 | `block_dynamic_sql=true` 설정으로 PREPARE/EXECUTE 자체를 차단하여 간접 완화 |

> 참조: `src/parser/procedure_detector.hpp:14-16`

---

## 위협 3: 프록시 자체 공격

**공격 목표**: dbgate 프록시 프로세스 자체를 무력화하거나 우회

### 3.1 악성 패킷 (Malformed Packets)

| 항목 | 설명 |
|------|------|
| 공격 시나리오 | MySQL 프로토콜 규격을 벗어나는 패킷으로 파서 크래시 유도 |
| 현재 상태 | 파싱 실패 시 `ParseError` → `PolicyEngine::evaluate_error()` → **kBlock 반환 (fail-close)**. 프로세스 크래시 방지를 위한 예외 처리 적용 |
| 위험도 | **중** |
| 완화 계획 | Fuzz 테스트(`tests/fuzz/`) 지속 확대 |

> 참조: `src/parser/sql_parser.hpp:77-79`, `docs/policy-engine.md:156-163`

### 3.2 DoS (Denial of Service)

| 항목 | 설명 |
|------|------|
| 공격 시나리오 | Slowloris 등 프로토콜 레벨 공격, 대량 연결로 리소스 고갈 |
| 현재 상태 | `MAX_CONNECTIONS` 제한 있으나, Slowloris 같은 저대역 공격 **미방어**. 쿼리 실행 시간 제한 미지원 |
| 위험도 | **중** |
| 완화 계획 | 연결별 타임아웃 강화, 쿼리 실행 시간 제한 추가 |

> 참조: `docs/architecture.md:955-957`

### 3.3 UDS 비인가 접근

| 항목 | 설명 |
|------|------|
| 공격 시나리오 | Unix Domain Socket에 무단 접근하여 통계 조회·정책 조작 시도 |
| 현재 상태 | UDS 소켓은 파일시스템 권한으로 보호 (`user: 1000:1000`). Docker 볼륨 격리 적용 |
| 위험도 | **하** |
| 완화 계획 | 현재 수준 유지. 필요 시 UDS 인증 토큰 추가 검토 |

### 3.4 TLS 관련 공격

| 항목 | 설명 |
|------|------|
| 공격 시나리오 | TLS 다운그레이드, 인증서 위조, 중간자 공격 |
| 현재 상태 | Frontend/Backend 독립 TLS 지원. OpenSSL 기반 Boost.Asio SSL 스트림 사용 |
| 위험도 | **중** |
| 완화 계획 | TLS 1.2+ 강제, 인증서 검증 강화 |

> 참조: `docs/adr/007-ssl-tls-async-stream.md`

---

## 위험 요약 매트릭스

| # | 위협 | 위험도 | 현재 상태 | 완화 계획 |
|---|------|--------|----------|----------|
| 1.1 | 주석 분할 우회 | 중 | 부분 완화 (DROP은 탐지, UNION은 미탐) | SQL 정규화 전처리 강화 |
| 1.2 | 인코딩 우회 | 중 | 미완화 | URL 디코딩, Hex 패턴 추가 |
| 1.3 | 대소문자 변형 | 하 | 완화됨 | - |
| 1.4 | 공백 변형 | 하~중 | 부분 완화 | 공백 정규화 강화 |
| 1.5 | Multi-Statement | 하 | 완화됨 (fail-close) | - |
| 2.1 | COM_STMT_PREPARE | 고 | 미파싱 | 바이너리 프로토콜 파싱 구현 |
| 2.2 | PREPARE/EXECUTE 동적 SQL | 중 | 조건부 완화 | block_dynamic_sql 기본 활성화 권장 |
| 2.3 | 변수 간접 참조 | 고 | 미완화 | block_dynamic_sql로 간접 차단 |
| 3.1 | 악성 패킷 | 중 | fail-close 적용 | Fuzz 테스트 확대 |
| 3.2 | DoS | 중 | 부분 완화 (MAX_CONNECTIONS) | 타임아웃·속도 제한 강화 |
| 3.3 | UDS 비인가 접근 | 하 | 파일시스템 권한 보호 | - |
| 3.4 | TLS 공격 | 중 | OpenSSL 기반 TLS 지원 | TLS 1.2+ 강제 |
| 4.1 | Monitor 모드 + 미등록 사용자 우회 | 고 | **해소** (DON-49 수정) | - |

---

## 위협 4: Monitor 모드 우회 (DON-49 수정)

**공격 목표**: `sql_rules.mode=monitor`(모니터 모드) 상태를 이용하여 미등록 사용자가 차단 대상 쿼리를 통과시킴

### 4.1 Monitor 모드 + 미등록 사용자 우회 (수정됨)

| 항목 | 설명 |
|------|------|
| 공격 시나리오 | `sql_rules.mode=monitor`로 설정 시, block_statements/block_patterns 매칭 후 즉시 kLog 반환하여 access_control의 no-access-rule 체크를 건너뜀 |
| 취약 버전 | DON-49 수정 전 |
| 수정 후 동작 | block_statement/block_pattern 매칭 시 즉시 반환하지 않고 `sql_rules_monitor_hit`에 저장 후 access_control 평가를 계속 진행. no-access-rule(미등록 사용자) 시 **kBlock 유지 (fail-close)**. access_control 통과 후에만 kLog 반환 |
| 위험도 | **고** (수정 전) → **해소** (수정 후) |

> 참조: `src/policy/policy_engine.cpp`, 테스트 `PolicyEngine.MonitorMode_NoAccessRule_WithBlockStatement_StillBlocks`

### 4.2 Monitor 모드 적용 범위

monitor 모드는 두 레벨에서 독립적으로 설정된다:

| 레벨 | 설정 위치 | 적용 대상 | no-access-rule 영향 |
|------|----------|----------|---------------------|
| `sql_rules.mode` | `policy.yaml: sql_rules.mode` | block_statements, block_patterns | **없음** — access_control 평가 후 kLog |
| `access_control[].mode` | 각 access rule의 `mode` | blocked_operations, allowed_operations, time_restriction 등 | 해당 없음 (룰이 매칭된 경우에만 적용) |

fail-close 원칙: 어느 레벨의 monitor 모드도 **no-access-rule(미등록 사용자)**에 대한 kBlock을 우회하지 않는다.

---

## Fail-Close 체크리스트

dbgate는 **fail-close** 원칙을 따른다. 불확실한 상황에서는 반드시 차단한다.

| 상황 | 동작 | 구현 위치 |
|------|------|----------|
| 정책 config가 null | kBlock 반환 | `PolicyEngine::evaluate()` |
| SQL 구문 분류 실패 (kUnknown) | kBlock 반환 | `PolicyEngine::evaluate()` |
| 정책 일치 없음 | kBlock 반환 (default deny) | `PolicyEngine::evaluate()` |
| 파서 오류 (ParseError) | kBlock 반환 (noexcept) | `PolicyEngine::evaluate_error()` |
| 엔진 내부 예외 | kBlock 반환 | `PolicyEngine::evaluate()` |
| InjectionDetector 유효 패턴 0개 | 모든 SQL 차단 | `InjectionDetector::check()` |
| block_patterns 비어있음 | 정책 로드 자체 실패 | `PolicyLoader::load()` |
| CIDR 파싱 실패 | 해당 룰 매칭 실패 → 다음 룰 평가 계속 (**주의**) | `PolicyEngine::evaluate()` |
| 시간 파싱/획득 실패 | kBlock 반환 | `PolicyEngine::evaluate()` |
| Multi-statement 감지 | ParseError 반환 → kBlock | `SqlParser::parse()` |
| Hot Reload에 nullptr 전달 | 이후 모든 evaluate()가 kBlock | `PolicyEngine::reload()` |
| sql_rules monitor 모드 + 미등록 사용자 | kBlock 반환 (access_control 평가 후) | `PolicyEngine::evaluate()` |

> **CIDR 파싱 실패 시 주의**: `ip_in_cidr()`가 `false`를 반환하면(CIDR 파싱 실패, IPv6 주소 등) 해당 룰의 매칭만 실패하고 `continue`로 다음 룰을 계속 평가한다. 이후에 `user: "*"` 같은 완화된 룰이 존재하면 해당 룰이 매칭되어 최종 kAllow가 될 수 있다. 이는 다른 fail-close 항목과 달리 **즉시 kBlock을 보장하지 않으므로**, `access_control` 룰 순서 배치에 주의해야 한다. 참조: `src/policy/policy_engine.cpp:407-409`

### 오탐/미탐 트레이드오프

```
보안 우선 ◀─────────── 현재 기본값 ────────────▶ 가용성 우선

패턴 확대                                      패턴 축소
  ↓                                              ↓
False Positive 증가                   False Negative 증가
(ORM 쿼리 오차단)                     (우회 공격 허용)
```

- **현재 기본값**: 보수적 (차단 우선). 일부 우회 가능하나 서비스 안정성 유지.
- 패턴은 `config/policy.yaml`의 `block_patterns`에서 운영자가 튜닝 가능.
- `InjectionResult::detected == true` → 정책 엔진이 `kBlock` 적용.
- 매칭된 패턴은 감사 로그에만 기록하며, 클라이언트에 노출하지 않는다 (공격자 피드백 최소화).

---

## 운영 권장사항

1. **`block_dynamic_sql: true`** — PREPARE/EXECUTE 기반 우회를 원천 차단 (합법적 Prepared Statement 사용 시 false positive 발생 가능)
2. **`block_schema_access: true`** — `information_schema`, `mysql`, `performance_schema`, `sys` 접근 차단
3. **`block_create_alter: true`** — 프로시저 생성/변경 차단
4. **정책 패턴 정기 검토** — 새로운 우회 기법 발견 시 `block_patterns`에 추가
5. **Fuzz 테스트 정기 실행** — `tests/fuzz/` 디렉토리의 퍼징 테스트로 파서 견고성 검증
6. **TLS 활성화** — 클라이언트 ↔ dbgate, dbgate ↔ MySQL 구간 모두 TLS 적용 권장
