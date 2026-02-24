# 문서 영향 정책 (Doc Impact Policy)

## 목적
- 코드/설정/테스트 변경 시 관련 문서가 함께 업데이트되도록 강제하는 운영 규칙을 정의한다.
- 사람/AI/CI가 동일한 기준으로 문서 누락을 판단할 수 있게 한다.

## 적용 범위
- `dbgate` 저장소의 구현/테스트/배포/도구 변경 작업 전반
- 대상 파일 유형:
  - `src/**`
  - `tools/**`
  - `tests/**`
  - `benchmarks/**`
  - `deploy/**`
  - `.github/workflows/**`
  - `docker-compose.yaml`
  - `config/**` (정책/설정 변경)

## 소스 오브 트루스
- 경로/소유권/문서 후보 매핑: `.claude/ownership-map.yaml`
- 상세 프롬프트 규칙: `.claude/agents/*.md`
- 완료 보고 형식: `docs/process/handoff-report-schema.md`

## 핵심 원칙
1. 구현/QA 에이전트는 코드 변경 시 `문서 영향 분석`을 반드시 보고한다.
2. 동작/인터페이스/운영 영향이 있으면 관련 문서를 같은 작업 흐름에서 1차 수정한다.
3. 문서를 수정하지 않은 경우, 사유를 명시한다.
4. `technical-writer`는 최종 정합성/표현/교차 문서 일관성을 검토한다.
5. CI는 문서 영향 누락을 자동 검출한다 (도입 후).

## 문서 영향 판단 기준

### 문서 수정이 필요한 경우 (기본)
- 외부 관찰 가능한 동작 변화 (`behavior`)
- 인터페이스/계약 변화 (`interface`)
- 운영 절차/설정/배포/로그/헬스체크 변화 (`ops`)
- 성능 특성/벤치마크 기준이 문서에 노출되는 변화 (`perf`, 필요 시)

### 문서 수정이 필요 없을 수 있는 경우
- `internal-refactor` (동작/인터페이스/운영 영향 없음)
- 테스트 데이터/픽스처만 변경
- 문서 자체 변경 (`docs-only`)

주의:
- "문서 수정 없음"은 허용될 수 있지만, `문서 영향 분석`과 예외 사유 보고는 생략할 수 없다.

## 경로별 기본 문서 후보 (요약)
정확한 규칙은 `.claude/ownership-map.yaml`을 따른다. 아래는 빠른 참고용 요약이다.

| 변경 경로 | 기본 문서 후보 |
|---|---|
| `src/protocol/**`, `src/proxy/**`, `src/health/**` | `docs/architecture.md`, `docs/data-flow.md`, `docs/sequences.md`, `docs/interface-reference.md` |
| `src/parser/**`, `src/policy/**`, `config/policy.yaml` | `docs/policy-engine.md`, `docs/data-flow.md`, `docs/interface-reference.md`, `docs/threat-model.md` |
| `src/logger/**`, `src/stats/**` | `docs/observability.md`, `docs/failure-modes.md`, `docs/uds-protocol.md`, `docs/interface-reference.md` |
| `tools/**` | `README.md`, `docs/runbook.md`, `docs/uds-protocol.md`, `docs/observability.md`, `docs/interface-reference.md` |
| `deploy/**`, `.github/workflows/**`, `docker-compose.yaml` | `docs/runbook.md`, `docs/failure-modes.md`, `README.md` |
| `tests/**`, `benchmarks/**` | `docs/testing-strategy.md`, `README.md` (필요 시) |

## 완료 보고 시 필수 항목
모든 구현/QA 에이전트는 아래 항목을 포함해야 한다.

- `문서 영향 분석`
  - `변경 동작/인터페이스/운영 영향`
  - `영향 문서 후보`
  - `실제 수정 문서`
  - `문서 미수정 사유(해당 시)`

상세 형식은 `docs/process/handoff-report-schema.md` 참고.

## 예외 처리 규칙 (commit trailer)
CI 자동 검사 예외는 commit trailer로만 허용한다.

필수 trailer:
- `Docs-Impact: none`
- `Docs-Impact-Reason: <구체적 사유>`

예시:

```text
Docs-Impact: none
Docs-Impact-Reason: internal refactor only, no behavior/interface/ops change
```

## CI 검사 정책 (도입 후 기준)

### 입력
- `git diff` 기준 변경 파일 목록
- `base`, `head` ref

### 검사 동작
1. 변경 파일 경로를 `.claude/ownership-map.yaml` 규칙에 매핑
2. 경로별 문서 후보 집합 계산
3. 실제 변경된 문서(`docs/**`, `README.md`)와 비교
4. 누락 시 실패 (예외 trailer 있으면 통과)

### 초기 운영 기준 (false positive 완화)
- 매핑된 문서 후보 그룹 중 "최소 1개" 문서가 변경되면 우선 통과
- 운영 중 누락이 반복되는 경우 stricter rule로 강화

## 승인 게이트 연계 (중요)
- `interface` 변경:
  - 문서 영향 분석 + 인터페이스 문서 갱신 검토 필수
  - 필요 시 `architect` 승인
- C++↔Go 경계 변경 (`src/stats/**` ↔ `tools/**`):
  - `architect` 승인 필수
  - `docs/uds-protocol.md` 갱신 검토 필수

## AI/호출자용 빠른 체크리스트
1. 변경 분류를 먼저 정했다 (`behavior/interface/ops/...`)
2. `.claude/ownership-map.yaml`에서 경로 규칙을 확인했다
3. 완료 보고에 `문서 영향 분석`을 넣었다
4. 문서 미수정이면 사유를 명시했다
5. 경계 계약 변경이면 architect 승인 여부를 명시했다
