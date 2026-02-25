# ADR-004: YAML 정책 파일 포맷 선택

## Status

Accepted

## Context

dbgate의 핵심 기능은 YAML 정책 파일을 읽어 SQL 요청을 평가하고 차단/허용을 결정하는 것이다.
정책 파일은 다음과 같은 특징을 갖는다:

1. **운영자 편집**: 개발자가 아닌 DBA, 보안팀이 직접 작성/수정해야 한다.
2. **계층 구조**: 전역 설정, 접근 제어, SQL 규칙, 프로시저 제어, 데이터 보호 등 다양한 섹션을 가진다.
3. **주석 필요성**: 각 규칙의 배경, 예외 사항을 설명하는 주석이 필수적이다.
4. **실시간 갱신**: 프록시 재시작 없이 정책을 hot reload할 수 있어야 한다.

정책 파일 포맷 선택 시 고려할 사항:
- 사람이 읽고 쓰기 쉬운 문법
- 중첩 구조 표현력
- 주석 지원
- 파싱 라이브러리의 성숙도 및 C++ 생태계 호환성
- DevOps 도구(Kubernetes, Ansible, Helm)와의 친숙성

## Decision

**YAML** 포맷을 선택하고, `yaml-cpp` 라이브러리로 파싱한다.

구체적으로:
- 정책 파일: `config/policy.yaml` (YAML 1.2 호환)
- 파싱 라이브러리: `yaml-cpp` (v0.7+)
- C++ 파싱 모듈: `src/policy/policy_loader.cpp`
- PolicyConfig 구조체: `src/policy/rule.hpp` (정책 필드 정의)

## Rationale (선택 이유)

### 운영자 편의성

YAML은 들여쓰기 기반 계층 구조로 복잡한 중첩을 직관적으로 표현한다.
예를 들어, 접근 제어 규칙(access_control)의 시간대 제한(time_restriction)을 표현할 때:

```yaml
# YAML 포맷 (읽기 쉬움)
access_control:
  - user: "readonly_user"
    source_ip: "192.168.1.0/24"
    allowed_tables: ["users", "orders", "products"]
    time_restriction:
      allow: "09:00-18:00"
      timezone: "Asia/Seoul"
```

반면 JSON은:

```json
{
  "access_control": [
    {
      "user": "readonly_user",
      "source_ip": "192.168.1.0/24",
      "allowed_tables": ["users", "orders", "products"],
      "time_restriction": {
        "allow": "09:00-18:00",
        "timezone": "Asia/Seoul"
      }
    }
  ]
}
```

TOML은:

```toml
[[access_control]]
user = "readonly_user"
source_ip = "192.168.1.0/24"
allowed_tables = ["users", "orders", "products"]

[access_control.time_restriction]
allow = "09:00-18:00"
timezone = "Asia/Seoul"
```

특히 운영자(비개발자)에게는 YAML의 들여쓰기 기반 구조가 가장 직관적이다.

### 주석 지원

YAML은 `#`을 사용한 주석을 공식 지원한다:

```yaml
# 금융 시스템 접근: 업무시간만 허용
access_control:
  - user: "finance_analyst"
    time_restriction:
      allow: "09:00-17:00"  # 점심(12-13시) 포함
      timezone: "Asia/Seoul"
```

JSON은 주석이 없으므로 정책 설명이 코드 주석으로만 가능하고, 설정 파일 자체에 의도를 남길 수 없다.

### 기존 생태계 호환성

- **Kubernetes**: ConfigMap/Secret의 YAML 기반 관리
- **Ansible**: 플레이북 작성에 YAML 사용
- **Helm**: Chart 설정값도 YAML 포맷
- **Terraform**: tfvar 파일에서도 YAML 호환 구문 지원

이미 DevOps 환경에서 YAML에 익숙한 운영팀이 dbgate 정책도 쉽게 관리할 수 있다.

### C++ 생태계: yaml-cpp 성숙도

`yaml-cpp` (https://github.com/jbeder/yaml-cpp)는:
- 순수 C++로 구현 (의존성 최소)
- C++11 이상 지원 (프로젝트의 C++23과 호환)
- vcpkg에서 배포 (cmake --preset 설정으로 자동 다운로드)
- 예외 기반 에러 처리로 C++ 스타일 통일
- YAML 1.2 명시 지원

실제 구현(`src/policy/policy_loader.cpp`)에서 yaml-cpp 사용:

```cpp
#include <yaml-cpp/yaml.h>

YAML::Node root = YAML::LoadFile(canonical_path.string());
cfg.global.log_level = read_string(global_node["log_level"], cfg.log_level);
cfg.access_control.push_back(parse_access_rule(rule_node));
```

### 성능

YAML 파싱은 정책 로드 시점(프록시 시작, hot reload)에만 발생하므로 성능 크리티컬이 아니다.
정책 평가(`PolicyEngine::evaluate`)는 파싱된 메모리 구조(PolicyConfig)를 참조하므로 런타임 오버헤드 없다.

## Consequences

### Positive

- **운영자 친화성**: 비개발자도 정책을 쉽게 작성/수정 가능
- **주석 지원**: 각 규칙의 의도/근거를 파일에 직접 기록 가능
- **계층 표현력**: 복잡한 중첩 구조(access_control → time_restriction)를 자연스럽게 표현
- **Hot Reload 친화적**: 정책 파일 변경 후 프록시 재시작 없이 즉시 적용 가능
- **기존 도구 활용**: Kubernetes ConfigMap, Ansible playbook 등 기존 DevOps 워크플로우와 통합 용이
- **라이브러리 성숙도**: yaml-cpp는 다양한 프로젝트에서 검증된 안정적인 라이브러리

### Negative (알려진 한계)

1. **들여쓰기 오류에 민감**
   - YAML의 들여쓰기(space vs tab, 2칸 vs 4칸)는 구조를 결정한다.
   - 잘못된 들여쓰기는 파싱 실패 또는 예상치 못한 구조를 유발한다.
   - 예: `allowed_tables` 열이 한 칸 부족하면 이전 항목에 속한다고 해석될 수 있다.
   - 완화: PolicyLoader가 상세한 에러 메시지(라인/열 번호)를 제공하여 디버깅 용이.

2. **앵커/앨리어스 복잡성**
   - YAML의 `&anchor`, `*alias`, `<<: *merge` 기능은 강력하지만 운영자에게 복잡할 수 있다.
   - 예: 공통 규칙을 재사용하려면 앵커 문법을 이해해야 함.
   - 현재 설계: 앵커/앨리어스 사용 필수 없음. 필요시 선택적으로 지원.

3. **타입 추론의 모호성**
   - YAML은 명시적 타입 선언이 없어 quoted vs unquoted 문자열이 혼동될 수 있다.
   - 예: `connection_timeout: 30s`는 문자열로 파싱되며, 정수 부분 추출은 PolicyLoader 헬퍼에서 처리.
   - 한계: `connection_timeout: "30s"` 형식이 강제되지 않아 운영자 실수 가능.
   - 대응: 정책 로드 시 타입 검증 및 폴백값 적용.

4. **대용량 파일 성능**
   - 매우 큰 정책 파일(수천 개 규칙)의 경우 파싱 시간이 증가할 수 있다.
   - 현재: 대부분의 프로덕션 환경에서 정책 규칙은 수십~수백 개 수준이므로 무시 가능.
   - 향후: 필요시 정책 분할(config inclusion) 또는 바이너리 캐시 도입.

## Alternatives Considered

### Option A: JSON

- **장점**: 표준성, 명시적 타입, 언어 간 호환성 높음
- **단점**: 주석 미지원, 중첩 구조에서 가독성 낮음, 개발자 친화적 (운영자 불친화)
- **결론**: 정책 파일은 운영자가 직접 편집해야 하므로 주석 지원이 필수 → JSON 제외

### Option B: TOML

- **장점**: 간결한 문법, 타입 명시적, 운영자 읽기 쉬움 (INI 유사)
- **단점**: 중첩 테이블(`[table.subtable]`) 표현이 다소 장황, 시퀀스(배열) 표현이 부자연스러움
- **예시**: access_control이 여러 규칙(배열)인 경우:
  ```toml
  [[access_control]]
  user = "rule1"

  [[access_control]]
  user = "rule2"
  ```
  이는 YAML의 다음과 비교하면:
  ```yaml
  access_control:
    - user: "rule1"
    - user: "rule2"
  ```
- **결론**: YAML이 배열 기반 구조를 더 자연스럽게 표현 → TOML 제외

### Option C: 커스텀 DSL (Domain Specific Language)

- **장점**: 정책에 최적화된 간결한 문법 가능 (예: 네트워크 ACL DSL)
- **단점**: 파서 직접 구현 필요, 문법 변경 시 마이그레이션 비용, 학습곡선 높음
- **결론**: 이미 검증된 YAML이 있으므로 DSL 구현은 오버엔지니어링 → 제외

## Policy Structure Example (참고)

실제 정책 파일의 구조(`config/policy.yaml` 기준):

```yaml
global:
  log_level: info
  log_format: json
  max_connections: 1000
  connection_timeout: 30s

access_control:
  - user: "admin"
    source_ip: "10.0.0.0/8"
    allowed_tables: ["*"]
    allowed_operations: ["*"]

  - user: "readonly_user"
    source_ip: "192.168.1.0/24"
    allowed_tables: ["users", "orders"]
    time_restriction:
      allow: "09:00-18:00"
      timezone: "Asia/Seoul"

sql_rules:
  block_statements:
    - DROP
    - TRUNCATE
  block_patterns:
    - "UNION\\s+SELECT"
    - "'\\s*OR\\s+'.*'\\s*=\\s*'"

procedure_control:
  mode: "whitelist"
  whitelist: ["sp_get_user"]
  block_dynamic_sql: true

data_protection:
  max_result_rows: 10000
  block_schema_access: true
```

## Implementation Details

### PolicyLoader 에러 처리

PolicyLoader::load는 다음과 같은 상황에서 오류를 반환한다:

1. **파일 미존재**: `std::filesystem::canonical` 실패
2. **YAML 문법 오류**: `YAML::ParserException` (라인/열 번호 포함)
3. **타입 미스매치**: `YAML::BadFile`, `YAML::Exception`
4. **필수 필드 누락**: `sql_rules.block_patterns` 비어있음 (fail-close 요구)
5. **정규식 패턴 검증**: 유효하지 않은 regex는 경고 로그만 출력 (파싱 실패 아님)

모든 에러는 `std::expected<PolicyConfig, std::string>` 패턴으로 반환되어 프록시 시작 시 명확한 오류 메시지를 제공한다.

### YAML ↔ C++ 필드 매핑

| YAML 키 | C++ 멤버 | 타입 | 기본값 |
|---------|----------|------|-------|
| `connection_timeout` | `connection_timeout_sec` | `uint32_t` | `30` |
| `time_restriction.allow` | `time_restriction.allow_range` | `string` | `"09:00-18:00"` |
| `source_ip` | `source_ip_cidr` | `string` | `""` |
| `block_patterns` | `block_patterns` | `vector<string>` | 필수 (빈값 오류) |

일부 YAML 키명과 C++ 멤버명이 다르다. PolicyLoader가 매핑을 담당하므로 운영자는 YAML 문서만 참조하면 된다.

## Hot Reload 고려사항

PolicyConfig는 `shared_ptr<PolicyConfig>`로 관리되어 PolicyEngine이 갱신 중에도 이전 정책으로 평가를 완료할 수 있다 (eventual consistency).
정책 파일을 수정하고 프록시에 SIGHUP 신호를 보내면 `PolicyLoader::load`가 즉시 새 파일을 읽고 PolicyEngine에 반영된다.
따라서 YAML 파일 형식이 hot reload를 방해하지 않는다.

## Future Work

- 정책 파일 분할 (inclusion): 대규모 환경에서 정책을 여러 파일로 나누어 관리
- 정책 검증 도구: 정책 파일 문법 및 정규식 유효성을 사전 검사하는 CLI 도구
- YAML 스키마 (JSON Schema): 정책 파일 구조를 자동 검증하고 IDE 자동완성 지원
- 정책 버전 관리: 정책 변경 이력 추적 및 롤백 기능

## References

- `src/policy/rule.hpp` — PolicyConfig 및 각 섹션 구조체 정의
- `src/policy/policy_loader.hpp` — PolicyLoader 인터페이스
- `src/policy/policy_loader.cpp` — yaml-cpp 기반 파싱 구현
- `config/policy.yaml` — 정책 파일 예시 (완전한 문법 참조)
- `src/policy/policy_engine.hpp` — 정책 평가 엔진 (PolicyConfig 사용)
