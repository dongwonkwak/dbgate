# 에이전트 프로세스 문서

## 목적
- `dbgate` 저장소에서 여러 서브에이전트가 어떻게 협업하는지 사람/AI가 빠르게 이해할 수 있게 한다.
- `.claude/agents/*.md`의 상세 규칙을 읽기 전에 전체 흐름/역할/보고 형식을 파악하게 한다.

## 이 문서 세트의 역할
- `docs/process/*`: 운영 방식 요약/가이드 (사람 + AI 온보딩용)
- `.claude/agents/*.md`: 각 에이전트의 상세 실행 규칙 (권한/금지사항/품질 기준의 원본)

## 먼저 읽을 순서 (권장)
1. `docs/process/agent-workflow.md` — 전체 작업 흐름
2. `docs/process/agent-catalog.md` — 에이전트 역할/소유권 요약
3. `docs/process/handoff-report-schema.md` — 완료 보고 형식
4. `.claude/agents/architect.md` — 설계/조율 규칙 원본
5. 담당 작업에 맞는 `.claude/agents/<agent>.md`

## 핵심 원칙 (요약)
1. `architect`가 설계/인터페이스를 확정하고 작업을 분배한다.
2. 구현/QA 에이전트는 코드 변경 시 테스트와 문서 영향 분석을 함께 수행한다.
3. `technical-writer`는 문서 품질/정합성 게이트 역할을 수행한다.
4. C++↔Go 경계(특히 UDS/JSON 계약) 변경은 `architect` 승인 후 반영한다.

## 현재 에이전트 구성 (요약)
- `architect`
- `network-engineer`
- `security-engineer`
- `infra-engineer`
- `go-engineer`
- `qa-engineer`
- `technical-writer`

## 소스 오브 트루스
- 에이전트 상세 규칙: `.claude/agents/*.md`
- 프로젝트 스펙: `docs/project-spec-v3.md`
- 저장소 운영 규칙: `CLAUDE.md`

## 문서 상태
- 이 디렉토리의 문서는 워크플로우 운영 문서이며, 구현 코드 인터페이스 문서는 아님
- 실제 인터페이스/프로토콜은 `docs/interface-reference.md`, `docs/uds-protocol.md`를 우선 참조
