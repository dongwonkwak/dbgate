# CLAUDE.md

## 프로젝트 개요
DB 접근제어 프록시. C++ 코어 + Go 운영도구.
MySQL 클라이언트와 서버 사이에 위치하여 SQL을 파싱하고 정책 기반으로 차단/허용/로깅을 수행한다.

## 아키텍처 규칙
- C++ 코어는 src/ 하위, Go 도구는 tools/ 하위
- C++ ↔ Go 통신은 반드시 Unix Domain Socket
- 새 파일 추가 시 CMakeLists.txt 업데이트 필수
- 모듈 의존 방향: common → protocol → parser → policy → proxy ← logger, stats
- 순환 의존 금지
- fail-close 원칙: 정책 엔진/검사 오류 시 반드시 차단 (fail-open 금지)
- 확정된 인터페이스(.hpp)를 architect 승인 없이 변경 금지

## Git Workflow
- 상세 규칙은 CONTRIBUTING.md 참조
- 커밋: `type(scope): 설명 [DON-XX]` (한글, Linear ID 필수)
- 브랜치: `feat/DON-XX-모듈명`, `fix/DON-XX-설명`
- PR 제목: 커밋 컨벤션과 동일 형식
- 머지: Squash merge → main (머지 후 브랜치 삭제)
- main에 직접 커밋 금지, 항상 PR 경유

## 서브에이전트 사용법
- 서브에이전트 정의: .claude/agents/ 디렉토리
- 호출: "[에이전트명] 서브에이전트를 사용해서 [작업]해줘"
- Claude Code가 작업 내용과 에이전트 description을 매칭하여 자동 위임하기도 함

### 에이전트 목록 및 역할
| 에이전트 | 역할 | 담당 경로 | Write 권한 |
|---------|------|----------|-----------|
| architect | 설계/조율 (plan mode) | - | ❌ |
| network-engineer | 프록시/프로토콜/헬스체크 | proxy/, protocol/, health/ | ✅ |
| security-engineer | SQL 파서/정책 엔진 | parser/, policy/ | ✅ |
| infra-engineer | 로거/통계/CI/배포 | logger/, stats/, deploy/, .github/ | ✅ |
| go-engineer | Go 컨트롤플레인/CLI/대시보드 | tools/ | ✅ |
| qa-engineer | 테스트/벤치마크 | tests/, benchmarks/ | ✅ |
| technical-writer | 문서 | docs/ | ✅ |

### 작업 흐름
1. architect에게 설계안/작업지시서 요청
2. architect 산출물을 기반으로 담당 engineer 서브에이전트 호출
3. 각 서브에이전트는 자기 담당 디렉토리 내에서만 작업
4. 인터페이스 변경이 필요하면 architect 승인 필수

## 프로세스 및 문서 참조
- `docs/process/execution-brief-template.md` — Execution Brief 작성 규칙
- `docs/process/state-transition-checklist.md` — 상태 전이 체크리스트

## Doc Impact 규칙

담당 경로를 수정하면 CI의 `scripts/check-doc-impact.sh`가 관련 문서 업데이트를 요구한다.
작업 완료 전 아래 중 하나를 **반드시** 수행해야 한다.

**옵션 A — 문서 업데이트**: 에이전트 정의의 "Doc Impact 후보" 목록에서 실제로 영향받는 파일을 1개 이상 수정하거나 신규 작성한다.

**옵션 B — 예외 트레일러**: 아래 허용 사유에 해당하면 마지막 커밋에 트레일러를 추가한다.
```
Docs-Impact: none
Docs-Impact-Reason: <사유>
```

허용 사유 (이 외에는 옵션 A 사용):
- `internal-refactor`: 공개 인터페이스/동작 변경 없는 내부 리팩터링
- `test-data-only`: 테스트 데이터/픽스처만 변경, 로직 변경 없음
- `pre-integration`: 해당 모듈이 아직 proxy에 연결되지 않아 문서 확정 전
- `doc-already-updated`: 같은 PR 내 다른 커밋에서 이미 문서를 업데이트함

남용 금지: 단순히 문서 작성이 귀찮다는 이유로 옵션 B를 선택하지 않는다.
