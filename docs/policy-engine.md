# 정책 엔진 (Policy Engine)

## 목적

`PolicyEngine`과 `PolicyLoader`는 dbgate의 핵심 접근제어 컴포넌트다.

- **PolicyLoader**: YAML 정책 파일(`config/policy.yaml`)을 읽어 `PolicyConfig` 구조체로 파싱한다.
- **PolicyEngine**: 파싱된 쿼리(`ParsedQuery`)와 세션 컨텍스트(`SessionContext`)를 받아 ALLOW/BLOCK/LOG 판정을 반환한다.

## 관련 문서

- `docs/interface-reference.md` — 인터페이스 레퍼런스 (rule.hpp, policy_loader.hpp, policy_engine.hpp)
- `docs/data-flow.md` — 정책 판정이 포함된 전체 데이터 흐름
- `docs/adr/004-yaml-policy-format.md` — YAML 정책 포맷 설계 결정

---

## Fail-Close 원칙

**정책 엔진은 불확실한 상황에서 반드시 차단한다.** 허용은 명시적 근거가 있을 때만 반환한다.

| 상황 | 동작 |
|------|------|
| `config_` == `nullptr` | `kBlock` 반환 |
| `SqlCommand::kUnknown` 쿼리 | `kBlock` 반환 |
| 정책 일치 없음 | `kBlock` 반환 (default deny) |
| 파서 오류 (`evaluate_error`) | `kBlock` 반환 (noexcept) |
| 엔진 내부 예외 | `kBlock` 반환 |
| CIDR 파싱 실패 | 해당 룰 매칭 실패 → 이후 `kBlock` |
| 시간 파싱 실패 | `kBlock` 반환 |
| 시간 획득 실패 (`localtime_r`) | `kBlock` 반환 |

`kAllow`는 아래 조건을 **모두** 통과했을 때만 반환한다.

---

## 평가 파이프라인 (evaluate)

`evaluate(query, session)` 호출 시 아래 순서를 **반드시** 준수한다. 앞 단계에서 `kBlock`이 결정되면 이후 단계는 실행하지 않는다.

### 1단계: config 유효성

`config_`가 `nullptr`이면 즉시 `kBlock` 반환.

### 2단계: Unknown 커맨드

`query.command == SqlCommand::kUnknown`이면 즉시 `kBlock` 반환.

### 3단계: SQL 구문 차단 (`block_statements`)

`config.sql_rules.block_statements` 목록과 `query.command`를 대소문자 무관 비교한다.
일치하면 `kBlock` / `matched_rule = "block-statement"`.

```yaml
# policy.yaml 예시
sql_rules:
  block_statements:
    - DROP
    - TRUNCATE
```

### 4단계: SQL 패턴 차단 (`block_patterns`)

`config.sql_rules.block_patterns`의 정규식 패턴을 `query.raw_sql`에 적용한다.
잘못된 regex 패턴은 경고 로그 후 **건너뜀** (해당 패턴의 false negative 증가).
일치하면 `kBlock` / `matched_rule = "block-pattern"`.

### 5단계: 접근 제어 룰 검색 (`access_control`)

`access_control` 배열에서 **순서대로** 첫 번째 매칭 룰을 찾는다.

매칭 조건:
- `rule.user == session.db_user` **또는** `rule.user == "*"` (와일드카드)
- `rule.source_ip_cidr`가 비어있거나 클라이언트 IP가 CIDR 범위 내

> **주의**: `user = "*"` 룰이 앞에 있으면 뒤의 특정 사용자 룰이 적용되지 않는다.

매칭 룰 없음 → `kBlock` / `matched_rule = "no-access-rule"`.

### 6단계: 차단 오퍼레이션 (`blocked_operations`)

매칭된 룰의 `blocked_operations`와 `query.command`를 비교한다.
`blocked_operations`는 `allowed_operations`보다 **우선** 적용된다.
일치하면 `kBlock` / `matched_rule = "blocked-operation"`.

### 7단계: 시간대 제한 (`time_restriction`)

매칭된 룰에 `time_restriction`이 설정된 경우:

- `allow_range` 파싱 실패 → `kBlock` (fail-close)
- 현재 시각이 허용 범위 밖 → `kBlock` / `matched_rule = "time-restriction"`

```yaml
# policy.yaml 예시
access_control:
  - user: readonly_user
    time_restriction:
      allow: "09:00-18:00"
      timezone: "Asia/Seoul"
```

> YAML 키는 `allow` (PolicyLoader가 `allow_range` 멤버로 매핑한다).

자정을 넘는 범위 (예: `22:00-06:00`)도 지원한다.

### 8단계: 테이블 접근 제어 (`allowed_tables`)

`allowed_tables`에 `"*"`가 없으면 `query.tables`의 각 테이블이 허용 목록에 있는지 확인한다.
허용되지 않은 테이블 → `kBlock` / `matched_rule = "table-denied"`.

`query.tables`가 비어있으면 이 단계를 건너뛴다 (허용).

### 9단계: 허용 오퍼레이션 (`allowed_operations`)

`allowed_operations`가 비어있지 않고 `"*"`도 없으면 `query.command`가 목록에 있는지 확인한다.
없으면 `kBlock` / `matched_rule = "operation-denied"`.

### 10단계: 프로시저 제어 (`procedure_control`)

`CALL`, `PREPARE`, `EXECUTE`, `CREATE`, `ALTER` 커맨드에 적용된다.

| 커맨드 | 조건 | 동작 |
|--------|------|------|
| `PREPARE` / `EXECUTE` | `block_dynamic_sql = true` | `kBlock` |
| `CALL` | 화이트리스트 모드 + 목록에 없음 | `kBlock` |
| `CALL` | 블랙리스트 모드 + 목록에 있음 | `kBlock` |
| `CREATE` / `ALTER` | `block_create_alter = true` | `kBlock` |

> 프로시저명은 `query.tables`의 첫 번째 요소에서 추출한다 (파서 구현 의존).

### 11단계: 스키마 접근 차단 (`block_schema_access`)

`data_protection.block_schema_access = true`이면 아래 스키마 접근을 차단한다:
- `information_schema`
- `mysql`
- `performance_schema`
- `sys`

**schema.table 형태 우회 방지**: SQL 파서가 `"information_schema.tables"` 형태로 테이블 토큰을 추출하는 경우에도 `.` 기준으로 앞부분(schema prefix)을 분리해 비교하므로 차단이 적용된다.

예시:
```sql
-- 파서 추출: tables = {"information_schema.tables"}
-- schema_part = "information_schema" → 차단
SELECT * FROM information_schema.tables
```

> **알려진 한계**: 파서가 토큰을 어떤 형태로 추출하는지에 의존한다. `db.schema.table` 3단계 형태나 백틱 이스케이프(`\`information_schema\`.tables`)는 처리하지 않는다.

### 12단계: 명시적 허용

위 모든 단계를 통과하면 `kAllow` / `matched_rule = "access-rule:<user>"` 반환.

---

## 파서 오류 처리 (evaluate_error)

파서가 `std::unexpected(ParseError)`를 반환하면 세션이 `evaluate_error(error, session)`을 호출한다.

- **반드시 `kBlock`** 반환 (`noexcept`)
- `matched_rule = "parse-error"`
- 오류 상세는 로그에만 기록하고 클라이언트에 노출하지 않는다

---

## PolicyLoader

### 로드 절차

`PolicyLoader::load(config_path)` 호출 시:

1. **경로 정규화**: `std::filesystem::canonical()`로 path traversal 방지
2. **YAML 파싱**: `yaml-cpp`로 파일 로드. `BadFile` / `ParserException` / `Exception` 각각 처리
3. **섹션별 파싱**: `global`, `access_control`, `sql_rules`, `procedure_control`, `data_protection` 순
4. **`block_patterns` 최소 1개 검증**: 비어있으면 `std::unexpected` 반환 (fail-close)
5. **패턴 유효성 사전 검증**: 잘못된 regex 경고 로그 출력

성공 시 `std::expected<PolicyConfig, std::string>`의 value 반환.
실패 시 `std::unexpected(error_message)` — **호출자는 반드시 기존 정책 유지 또는 차단 처리해야 한다.**

### YAML 키 매핑

| YAML 키 | C++ 멤버 |
|---------|---------|
| `access_control[].source_ip` | `AccessRule::source_ip_cidr` |
| `access_control[].time_restriction.allow` | `TimeRestriction::allow_range` |
| `global.connection_timeout` | `GlobalConfig::connection_timeout_sec` (숫자만 추출, `"30s"` → `30`) |

### block_patterns 필수 이유

`block_patterns`가 비어있으면 `InjectionDetector`가 `fail_close_active_ = true` 상태가 되어 **모든 SQL을 차단**한다. 이 상태를 운영자 실수로 만드는 것을 방지하기 위해 로드 시점에 명시적 오류를 반환한다.

---

## Hot Reload

`PolicyEngine::reload(new_config)` 호출 시 `config_` 멤버를 원자적으로 교체한다.

**thread-safety 보장**: `config_`는 `std::atomic<std::shared_ptr<PolicyConfig>>` (C++20)로 선언되어 있다. `reload()`는 `store(memory_order_release)`, `evaluate()`는 `load(memory_order_acquire)`를 사용하여 acquire-release 쌍으로 동기화된다. 진행 중인 `evaluate()`는 교체 이전의 config 포인터를 계속 안전하게 참조한다(shared_ptr 참조 카운트 보장).

`new_config = nullptr`로 reload하면 이후 모든 `evaluate()`가 `kBlock`을 반환한다 (fail-close).

---

## 알려진 한계

| 항목 | 설명 |
|------|------|
| IPv6 CIDR 미지원 | `ip_in_cidr()`는 IPv4 전용. IPv6 주소는 `false` 반환 (fail-close). |
| DST 오차 | `std::chrono::zoned_time` (IANA tz database) 기반 시간대 처리. DST 전환을 정확하게 처리한다. |
| 테이블명 추출 불완전 | 복잡한 서브쿼리/CTE에서 파서가 내부 테이블명을 놓칠 수 있음. |
| 주석 분할 미탐지 | `UN/**/ION` 형태는 `block_patterns`로 탐지 불가. |
| Hot Reload thread-safety | `std::atomic<std::shared_ptr<PolicyConfig>>`로 thread-safe 교체 보장. |
| 시간대 이름 형식 | IANA 표준 이름(`"Asia/Seoul"`)만 지원. `"KST+9"` 형태 불가. |
| schema.table 3단계 형태 | `db.schema.table` 3단계 형태나 백틱 이스케이프는 처리하지 않음 (파서 한계). |
| procedure_control 프로시저명 | `query.tables`의 첫 번째 요소에서 추출. 파서 한계에 따라 빈 문자열 가능. |

---

## 운영 주의사항

1. **`access_control` 순서**: 먼저 정의된 룰이 우선 적용된다. `user = "*"` 룰을 상단에 두면 이후 사용자별 룰이 무시된다.
2. **`block_patterns` 최소 1개 필수**: 비워두면 정책 로드 자체가 실패한다.
3. **`blocked_operations`와 `allowed_operations`**: 두 목록에 같은 커맨드가 있으면 `blocked_operations`가 우선 적용된다.
4. **regex 패턴 검증**: 잘못된 패턴은 해당 패턴의 탐지가 누락된다. 로드 시 경고 로그를 확인할 것.
5. **`procedure_control.mode`**: `"whitelist"` 또는 `"blacklist"` 외의 값은 `"whitelist"`로 강제 설정된다.
