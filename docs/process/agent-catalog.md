# 서브에이전트 카탈로그 (Agent Catalog)

## 목적
- 각 서브에이전트의 책임, 담당 경로, 금지사항, 협업 대상을 빠르게 파악하기 위한 요약 문서
- 상세 규칙은 `.claude/agents/*.md`를 참조한다

## 요약 표

| Agent | 주 책임 | 담당 경로(요약) | 주 협업 대상 | 상세 규칙 |
|---|---|---|---|---|
| `architect` | 설계/인터페이스 확정, 작업 분배, 통합 판단 | 구현 직접 수정 없음 | 전원 | `.claude/agents/architect.md` |
| `network-engineer` | C++ 네트워크/프록시 구현 | `src/protocol/`, `src/proxy/`, `src/health/` | `security`, `infra`, `qa` | `.claude/agents/network-engineer.md` |
| `security-engineer` | SQL 파서/정책 엔진 구현 | `src/parser/`, `src/policy/` | `network`, `infra`, `qa` | `.claude/agents/security-engineer.md` |
| `infra-engineer` | 로깅/통계/배포/CI | `src/logger/`, `src/stats/`, `deploy/`, `.github/workflows/` | `go`, `qa`, `writer` | `.claude/agents/infra-engineer.md` |
| `go-engineer` | Go 컨트롤플레인/CLI/대시보드 | `tools/` | `infra`, `qa`, `writer` | `.claude/agents/go-engineer.md` |
| `qa-engineer` | 테스트/퍼징/통합/벤치마크 | `tests/`, `benchmarks/` | 구현 에이전트 전원, `architect` | `.claude/agents/qa-engineer.md` |
| `technical-writer` | ADR/기술 문서/정합성 검토 | `docs/`, `README.md` | `architect`, 구현 에이전트, `qa` | `.claude/agents/technical-writer.md` |

## 에이전트별 요약

### `architect`
- 역할:
  - 인터페이스/모듈 경계 확정
  - 작업 분배/병렬화 계획
  - 통합 시 인터페이스 위반 판단
- 금지:
  - 구현 코드 직접 작성/수정
- 중요 규칙:
  - C++↔Go 경계 변경은 architect 승인 필요

### `network-engineer`
- 역할:
  - Boost.Asio 기반 프록시/프로토콜/헬스체크 구현
- 강점 영역:
  - 비동기 I/O, 세션 상태 전이, 패킷 경계 처리
- 자주 협업:
  - `security-engineer` (정책 적용 지점)
  - `infra-engineer` (stats/logging hook)
  - `qa-engineer` (엣지 케이스 테스트)

### `security-engineer`
- 역할:
  - SQL 파서/탐지/정책 엔진 구현
- 강점 영역:
  - 우회 가능성 분석, fail-close, 오탐/미탐 트레이드오프
- 자주 협업:
  - `network-engineer` (파싱 호출 흐름)
  - `qa-engineer` (우회 테스트)
  - `technical-writer` (한계 문서화)

### `infra-engineer`
- 역할:
  - C++ 로깅/통계, 배포 구성, CI 파이프라인 운영
- 강점 영역:
  - 재현 가능성, 자동화, 운영성
- 자주 협업:
  - `go-engineer` (UDS 경계/도구 연계)
  - `technical-writer` (runbook/observability)

### `go-engineer`
- 역할:
  - `tools/` 기반 CLI/대시보드/UDS client 구현
- 강점 영역:
  - Go `context`, goroutine 종료 경로, CLI UX
- 자주 협업:
  - `infra-engineer` (`src/stats`/UDS 연계)
  - `qa-engineer` (CLI/통합 테스트)
  - `technical-writer` (README/runbook/UDS 문서)

### `qa-engineer`
- 역할:
  - 단위/퍼징/통합/벤치마크 테스트 설계 및 결함 재현
- 강점 영역:
  - 실패 재현, 경계값, 동시성/메모리 안전성 검증
- 자주 협업:
  - 구현 에이전트 전원 (결함 수정 루프)
  - `architect` (테스트성/관측성 개선 제안)

### `technical-writer`
- 역할:
  - ADR/README/기술 문서 작성 및 정합성 유지
- 강점 영역:
  - 용어 통일, 교차 문서 검증, 트레이드오프 문서화
- 자주 협업:
  - `architect` (설계 확정 반영)
  - 구현 에이전트/QA (사실관계 검증)

## 경계/협업 포인트 (중요)

### C++↔Go 경계
- 주 대상:
  - `src/stats/` (UDS server)
  - `tools/` (UDS client)
- 참여 에이전트:
  - `infra-engineer`, `go-engineer`, `architect`
- 규칙:
  - 계약 변경은 `architect` 승인 후 반영

### 문서 반영
- 구현자가 먼저 관련 문서를 1차 수정
- `technical-writer`가 정합성/표현/누락 검토

## AI 사용 팁
- 작업 전: 이 문서 -> 담당 에이전트 프롬프트 순서로 읽는다
- 작업 후: `docs/process/handoff-report-schema.md` 형식에 맞춰 보고한다
