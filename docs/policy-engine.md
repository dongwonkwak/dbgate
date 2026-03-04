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

## Monitor Mode (단계적 배포)

Monitor mode는 새로운 차단 정책을 실제 차단 없이 먼저 테스트할 수 있는 기능입니다. 정책 규칙에서 차단 판정이 나더라도 로그만 기록하고 실제 쿼리는 허용합니다.

### 용도

- **정책 변경 검증**: 새 차단 규칙을 프로덕션에 적용하기 전에 "만약 적용되면 어떤 쿼리들이 차단되는가"를 확인
- **거짓 양성 감지**: 정책이 의도치 않게 정상 쿼리를 차단하지는 않는지 확인
- **점진적 롤아웃**: 정책을 enforce mode로 전환하기 전에 문제점 파악

### 설정 방법

Monitor mode는 두 가지 레벨에서 설정할 수 있습니다.

#### 1. 접근 제어 규칙 (`access_control`) — rule-level monitor mode

개별 사용자/IP 규칙에만 monitor mode를 적용할 수 있습니다:

```yaml
access_control:
  - user: app_service
    source_ip: 192.168.1.0/24
    mode: monitor      # 이 규칙만 monitor 모드
    allowed_tables: ["users", "orders"]
    allowed_operations: ["SELECT", "INSERT"]

  - user: readonly_user
    mode: enforce      # (기본값) 일반 차단 모드
    allowed_tables: ["*"]
    allowed_operations: ["SELECT"]
```

#### 2. SQL 규칙 (`sql_rules`) — section-level monitor mode

모든 SQL 구문/패턴 차단 규칙에 monitor mode를 적용:

```yaml
sql_rules:
  mode: monitor        # 전체 SQL 룰 모니터링
  block_statements:
    - DROP
    - TRUNCATE
    - GRANT
  block_patterns:
    - "(?i)union.*select"
    - "(?i)select.*from.*where.*or.*=.*"
```

### Monitor Mode 동작

Monitor mode가 적용된 규칙이 일치하면:

1. **판정 다운그레이드**: `PolicyAction::kBlock` → `PolicyAction::kLog`
2. **플래그 설정**: `PolicyResult.monitor_mode = true`
3. **로그 기록**: 로그에 `[monitor]` 프리픽스 추가
4. **실제 차단 없음**: 쿼리는 **허용**되어 클라이언트에 전달됨

**예시**:
```cpp
PolicyResult result = engine.evaluate(query, session);

// Monitor mode 규칙에 매칭된 경우:
// - result.action == PolicyAction::kLog
// - result.monitor_mode == true
// - result.matched_rule == "access-rule:app_service"
// - result.reason == "[monitor] Operation denied: SELECT"
// - 쿼리는 차단되지 않음
```

### 예외: 일부 규칙은 Monitor Mode 영향을 받지 않음

#### No-Access-Rule (기본값 거부)

매칭된 access_control 규칙이 없는 경우 발생합니다. Monitor mode와 관계없이 **항상 차단**됩니다:

```yaml
access_control:
  - user: admin
    source_ip: 10.0.0.0/8
    allowed_tables: ["*"]
```

```sql
-- 이 SQL은 위 규칙을 매칭하지 않는 사용자가 실행
-- 결과: PolicyAction::kBlock (monitored_blocks 가 아님)
SELECT * FROM users;
```

#### 파서 오류 (`evaluate_error`)

SQL 파싱 실패로 인한 차단은 monitor mode와 무관하게 **항상 차단**됩니다:

```cpp
// 파서 오류 발생 시
if (!parsed) {
    result = engine.evaluate_error(error, session);
    // 항상 result.action == PolicyAction::kBlock
    // monitor_mode 플래그는 설정되지 않음
}
```

### 통계 추적

Monitor mode로 차단된 쿼리는 별도 카운터로 추적됩니다:

| 카운터 | 의미 | Monitor Mode 영향 |
|--------|------|------------------|
| `blocked_queries` | 실제로 차단된 쿼리 | 미포함 (monitor 규칙 무관) |
| `monitored_blocks` | Monitor mode로 "차단되었을" 쿼리 | 포함 (block_rate 영향 X) |

**block_rate 정확성**:
```
block_rate = blocked_queries / total_queries
           (monitored_blocks 제외)
```

Monitor mode 정책의 영향을 모니터링하면서도 실제 차단율 지표는 왜곡되지 않도록 설계되었습니다.

### 로그 형식

Monitor mode로 차단된 쿼리는 block log에서 식별할 수 있습니다:

```json
{
  "timestamp": "2026-03-04T10:15:00Z",
  "event": "query_would_block",
  "user": "app_service",
  "source_ip": "192.168.1.100",
  "raw_sql": "DROP TABLE users",
  "action": "log",
  "matched_rule": "block-statement",
  "monitor_mode": true,
  "would_block": true,
  "reason": "[monitor] SQL statement blocked: DROP"
}
```

주요 필드:
- `event`: `"query_would_block"` (실제 차단이 아님을 명시)
- `action`: `"log"` (차단이 수행되지 않음)
- `monitor_mode`: `true` (monitor mode 규칙에 의한 기록)
- `would_block`: `true` (enforce mode였다면 차단됐을 것)

### 배포 패턴

#### 패턴 1: 신규 규칙 검증

```yaml
# 1단계: monitor mode로 배포
access_control:
  - user: app_service
    mode: monitor
    blocked_operations: ["DROP", "TRUNCATE"]

# 검증 기간: 1-2주 동안 monitored_blocks 모니터링
# 통계 쿼리:
#   SELECT monitored_blocks, blocked_queries FROM stats;
# 거짓 양성이 없으면 다음 단계 진행

# 2단계: enforce mode로 전환
access_control:
  - user: app_service
    mode: enforce      # monitor → enforce
    blocked_operations: ["DROP", "TRUNCATE"]
```

#### 패턴 2: 점진적 롤아웃

```yaml
access_control:
  - user: internal_user
    mode: enforce      # 기존 규칙: 강력하게 적용
    blocked_operations: ["DROP"]

  - user: app_service
    mode: monitor      # 신규 규칙: 먼저 모니터링
    blocked_operations: ["DROP", "TRUNCATE", "ALTER"]
```

### 정책 설명 API (`policy_explain`)

`policy_explain` 커맨드의 응답에도 `monitor_mode` 플래그가 포함됩니다:

```json
{
  "ok": true,
  "payload": {
    "action": "log",
    "matched_rule": "block-statement",
    "reason": "[monitor] SQL statement blocked: DROP",
    "monitor_mode": true,
    "evaluation_path": "config_loaded > block_statement(DROP)"
  }
}
```

이를 통해 CLI 또는 대시보드에서 "이 정책은 현재 monitor mode로 실행 중"임을 명시적으로 표시할 수 있습니다.

---

## 파서 오류 처리 (evaluate_error)

파서가 `std::unexpected(ParseError)`를 반환하면 세션이 `evaluate_error(error, session)`을 호출한다.

- **반드시 `kBlock`** 반환 (`noexcept`)
- `matched_rule = "parse-error"`
- 오류 상세는 로그에만 기록하고 클라이언트에 노출하지 않는다

---

## 정책 설명 API (explain / explain_error)

`explain()` / `explain_error()`는 **디버깅/감사 목적 전용** dry-run API다. 실제 차단/허용 동작은 수행하지 않고 `ExplainResult`만 반환한다.

> **데이터패스에서 사용 금지**: 프로덕션 데이터패스(proxy 레이어)에서는 반드시 `evaluate()` / `evaluate_error()`를 사용해야 한다. `explain()`은 실제 차단을 수행하지 않는다.

### ExplainResult 구조

| 필드 | 타입 | 설명 |
|------|------|------|
| `action` | `PolicyAction` | 판정 결과 (`kAllow` / `kBlock`). 기본값 `kBlock` (fail-close) |
| `matched_rule` | `std::string` | 매칭된 규칙 식별자 (evaluate 결과와 동일) |
| `reason` | `std::string` | 판정 이유 (로깅용, 클라이언트 노출 금지) |
| `matched_access_rule` | `std::string` | `"user@cidr"` 형식. access_control 룰 매칭 이후 단계에서만 채워짐 |
| `evaluation_path` | `std::string` | `">"` 구분 단계별 평가 경로 trace |

### evaluation_path 예시

허용 경로:
```
config_loaded > command_known(SELECT) > block_statements_passed > block_patterns_passed
  > access_rule_matched(testuser@192.168.1.0/24) > blocked_operations_passed
  > tables_passed > operation_allowed(SELECT) > access_allowed(SELECT)
```

차단 경로 (DROP 구문 차단):
```
config_loaded > command_known(DROP) > block_statement(DROP)
```

차단 경로 (파서 오류):
```
parse_failed
```

### fail-close 원칙 유지

`explain()` / `explain_error()`도 `evaluate()` / `evaluate_error()`와 동일하게 fail-close를 준수한다.

| 상황 | action |
|------|--------|
| `config_` == `nullptr` | `kBlock` / `evaluation_path = "config_null"` |
| `SqlCommand::kUnknown` | `kBlock` |
| 파서 오류 (`explain_error`) | `kBlock` / `evaluation_path = "parse_failed"` (noexcept) |
| 모든 오류/예외 | `kBlock` |

### evaluate()와의 결과 일관성

`explain()`의 `action`과 `matched_rule`은 동일 입력에 대해 `evaluate()`와 반드시 일치한다. 로직을 공유하므로 판정 결과가 다를 경우 버그를 의미한다.

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
