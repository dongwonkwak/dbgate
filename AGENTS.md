# AGENTS.md

> 공유 규칙 — Claude Code와 Codex CLI 모두 이 파일을 읽는다.
> Claude Code 전용 설정(서브에이전트, Docker 보안 등)은 `CLAUDE.md` 참조.

---

## 프로젝트 개요

DB 접근제어 프록시. C++ 코어 + Go 운영도구.
MySQL 클라이언트와 서버 사이에 위치하여 SQL을 파싱하고 정책 기반으로 차단/허용/로깅을 수행한다.

---

## 아키텍처 규칙

- C++ 코어는 `src/` 하위, Go 도구는 `tools/` 하위
- C++ ↔ Go 통신은 반드시 Unix Domain Socket
- 새 파일 추가 시 `CMakeLists.txt` 업데이트 필수
- 모듈 의존 방향: `common → protocol → parser → policy → proxy ← logger, stats`
- 순환 의존 금지
- **fail-close 원칙**: 정책 엔진/검사 오류 시 반드시 차단 (fail-open 금지)
- 확정된 인터페이스(`.hpp`)를 architect 승인 없이 변경 금지

---

## Git Workflow

- 상세 규칙은 `CONTRIBUTING.md` 참조
- 브랜치: `feat/DON-XX-모듈명`, `fix/DON-XX-설명`
- PR 제목: 커밋 컨벤션과 동일 형식
- PR 본문: `.github/pull_request_template.md` 템플릿을 반드시 사용
- 머지: Squash merge → main (머지 후 브랜치 삭제)
- main에 직접 커밋 금지, 항상 PR 경유

### 커밋 메시지 형식 (CI 검증 패턴과 동일)

```
type(scope): 설명 [DON-XX]
```

- **type**: `feat` | `fix` | `docs` | `chore` | `test` | `refactor` | `perf` | `style` | `ci` | `revert`
- **scope**: 소문자 영문+하이픈만 허용 (`[a-z][a-z0-9-]*`). **콤마(,)로 여러 스코프 나열 금지** — 대표 스코프 하나만 사용
- **설명**: 한글 권장, 마침표 없이
- **[DON-XX]**: Linear 이슈 ID 필수

올바른 예:
```
fix(logger): sign-conversion 경고로 인한 CI 빌드 실패 수정 [DON-30]
feat(proxy): 세션 타임아웃 처리 추가 [DON-45]
```

잘못된 예:
```
fix(logger,stats): 수정 [DON-30]   ← 콤마 사용 금지
Fix(Logger): 수정 [DON-30]         ← 대문자 금지
fix(logger): 수정                   ← Linear ID 누락
```

---

## 테스트 데이터 규칙

- 테스트 데이터 파일 생성 시 `docs/test-data-guidelines.md` 규칙을 따른다
- 코퍼스 파일 추가/수정/삭제 시 해당 디렉토리의 `DATA_CATALOG.yaml`을 반드시 업데이트한다
- 네이밍: `[category]_[description].[ext]` (소문자 + 언더스코어만 허용)
- 금지: 해시명, `temp_*`, `tmp_*`, `untitled*`, 대문자, 공백
- pre-commit 훅이 네이밍 + 카탈로그 동기화를 검증한다

---

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

---

## Review Guidelines

- 리뷰는 반드시 한글로 작성할 것
- P0/P1 이슈만 플래그할 것

### 보안
- 버퍼 오버플로우 가능성
- 사용자 입력 검증 누락
- SQL 파서 우회 가능 경로

### 성능
- 불필요한 복사 (move 가능한 곳)
- 핫패스에서의 메모리 할당
- Boost.Asio 비동기 패턴 오용

### C++ 품질
- RAII 위반
- 예외 안전성
- 스레드 안전성 (strand 밖에서 공유 자원 접근)

### Go 품질
- 에러 처리 누락
- goroutine 누수 가능성
- context 전파 누락

### 테스트
- 빠진 엣지 케이스
- 테스트가 실제 동작을 검증하는지
