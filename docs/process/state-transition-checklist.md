# 상태 전이 체크리스트 v1

## 목적
- Linear 이슈의 상태 전이를 "감"이 아니라 체크 가능한 증거로 수행한다.
- 모든 전이는 사람이 승인한다 (반자동).

## 적용 범위
- `dbgate` 프로젝트의 모든 작업 이슈 (DON-XX)
- 우선 `In Progress → In Review` 게이트만 정의한다.
- 추후 필요 시 다른 전이 게이트를 추가한다.

## 전이: In Progress → In Review

### 필수 증거 체크리스트

| # | 항목 | 확인 위치 | 비고 |
|---|---|---|---|
| 1 | Execution Brief 코멘트 존재 | Linear 이슈 코멘트 (`📋`) | v1 이상 |
| 2 | Handoff Report 존재 | Linear 이슈 코멘트 (`✅`) | 기존 schema 형식 |
| 3 | 테스트/검증 결과 존재 | Handoff Report 내 | 빌드/테스트 통과 근거 |
| 4 | 문서 영향 분석 존재 | Handoff Report 내 | `없음`이라도 명시 필수 |
| 5 | (해당 시) Architect 승인 근거 | Linear 코멘트 또는 대화 기록 | interface/경계 변경일 때만 |

### 판단 기준

**전이 가능**:
- 1~4번 모두 충족
- 5번은 변경 유형이 `interface`이거나 C++↔Go 경계 변경일 때만 적용

**전이 보류**:
- 1~4번 중 하나라도 미충족 시 전이하지 않음
- 미충족 항목을 보완한 후 재확인

### 변경 유형별 적용 강도

| 변경 유형 | Brief 필수 | Handoff 필수 | 테스트 근거 | 문서 영향 | Architect 승인 |
|---|---|---|---|---|---|
| `behavior` | 표준 | ✅ | ✅ | ✅ | 해당 시 |
| `interface` | 표준 | ✅ | ✅ | ✅ | ✅ 필수 |
| `ops` | 표준 | ✅ | ✅ | ✅ | 해당 시 |
| `perf` | 표준 | ✅ | ✅ (벤치마크 포함) | ✅ | 해당 시 |
| `internal-refactor` | 경량 | ✅ | ✅ | `none` + 사유 | 불필요 |
| `docs-only` | 경량 | ✅ (간소) | 해당 시 | 해당 없음 | 불필요 |

## 전이: Todo → In Progress

현재는 게이트를 두지 않는다. 향후 필요 시 아래 최소 조건을 도입할 수 있다:
- Execution Brief 코멘트 존재 (권장, 필수 아님)

## 전이: In Review → Done

현재는 게이트를 두지 않는다. 향후 필요 시 아래 조건을 도입할 수 있다:
- 리뷰 반영 확인
- 최종 머지/브랜치 정리
- 후속 이슈 분리 여부 확인

## 운영 방식
- 자동 상태 변경 아님. 사람이 체크 후 수동 전이한다.
- 체크리스트 미충족 시 Linear 코멘트에 미충족 사유를 남기고 보류한다.
- 서브에이전트가 체크리스트 충족 여부를 점검/제안할 수 있으나, 전이 자체는 사람이 한다.

## 관련 문서
- `docs/process/execution-brief-template.md` — Brief 작성 규칙
- `docs/process/handoff-report-schema.md` — Handoff Report 형식
- `docs/process/agent-workflow.md` — 전체 작업 흐름 (이 체크리스트가 삽입되는 위치)
- `docs/process/doc-impact-policy.md` — 문서 영향 판단 기준