# CLAUDE.md

> Claude Code 전용 설정. 프로젝트 공유 규칙(아키텍처, Git, 테스트 데이터, Doc Impact 등)은 `AGENTS.md` 참조.

---

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

## Docker/배포 보안 규칙
- 시크릿(비밀번호, 토큰, 인증정보)은 YAML/코드에 **평문 하드코딩 금지** → `env_file` 또는 docker secret 사용하고 `.env.example`만 커밋
- 내부 전용 서비스 포트(DB, 관리 UI, stats 등)는 **`127.0.0.1` 바인딩** 또는 포트 미노출 — `"3306:3306"` 같은 와일드카드 바인딩 금지
- 관리/모니터링 엔드포인트(stats, admin 등)는 **반드시 인증 + 접근 제한** 적용
- `.dockerignore`에 시크릿 패턴(`.env*`, `*.pem`, `*.key`) **필수 포함**
- TCP 프록시 타임아웃은 대상 프로토콜 특성 고려 (MySQL `wait_timeout=28800s` 기준, 최소 300s 이상)
- 배포 시 활성 세션 즉시 종료(`shutdown-sessions`) 지양 → 신규 유입 차단 후 자연 드레인 우선
