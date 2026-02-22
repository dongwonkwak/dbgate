# Contributing Guide

이 문서는 `dbgate` 저장소의 기본 기여 규칙과 커밋/PR/브랜치 컨벤션을 정의합니다.

---

## 브랜치 네이밍 규칙

```text
<type>/<모듈명 또는 주제>
```

| type | 용도 | 예시 |
|------|------|------|
| `feat/` | 새 기능 | `feat/mysql-protocol`, `feat/sql-parser` |
| `fix/` | 버그 수정 | `fix/packet-overflow` |
| `docs/` | 문서 | `docs/adr-002` |
| `chore/` | 빌드/CI/설정 | `chore/ci-pipeline` |
| `test/` | 테스트 추가 | `test/fuzz-parser` |
| `refactor/` | 리팩터링 | `refactor/session-cleanup` |

규칙:

- 소문자 + 하이픈(`-`) 사용
- 브랜치명에 Linear 이슈 ID를 포함하면 자동 연결됨 (예: `feat/DON-24-mysql-protocol`)
- `main`에 직접 커밋하지 않는다. 항상 PR을 통해 머지한다.

---

## 커밋 컨벤션 (필수)

모든 커밋 메시지는 아래 형식을 따릅니다.

```text
<type>(scope): 설명 [DON-XX]
```

- `type`: 변경 유형 (필수)
- `scope`: 모듈명 (필수)
- `설명`: 변경 내용 요약, **한글로 작성** (필수)
- `[DON-XX]`: Linear 이슈 ID (필수)

예시:

```text
feat(protocol): MySQL 패킷 파서 구현 [DON-24]
fix(policy): YAML 파싱 시 빈 파일 크래시 수정 [DON-26]
test(parser): SQL Injection 우회 케이스 추가 [DON-34]
docs(adr): 001-boost-asio-over-raw-epoll 작성 [DON-20]
chore(ci): GitHub Actions 파이프라인 추가 [DON-30]
```

### type 목록 (허용 값)

| type | 용도 |
|------|------|
| `feat` | 새 기능 |
| `fix` | 버그 수정 |
| `test` | 테스트 추가/수정 |
| `docs` | 문서 |
| `chore` | 빌드/CI/설정 |
| `refactor` | 리팩터링 |

### scope 목록

| scope | 담당 영역 |
|-------|----------|
| `protocol` | MySQL 프로토콜 파싱/핸드셰이크 |
| `proxy` | Boost.Asio 프록시 서버/세션 |
| `health` | Health Check |
| `parser` | SQL 파서/Injection 탐지/프로시저 탐지 |
| `policy` | 정책 엔진/YAML 로딩/Hot Reload |
| `logger` | 구조화 로거 |
| `stats` | 통계/UDS 서버 |
| `cli` | Go CLI 도구 |
| `dashboard` | 웹 대시보드 |
| `ci` | GitHub Actions/CI 파이프라인 |
| `deploy` | Docker/HAProxy/배포 |
| `adr` | Architecture Decision Records |
| `agents` | Claude Code 서브에이전트 |
| `project` | 프로젝트 설정 (devcontainer, CMake 등) |

새 모듈이 추가되면 이 목록도 업데이트합니다.

### 설명 규칙

- **한글로 작성**한다 (코드, 명령어, 기술 용어는 영어 유지)
- 변경 내용을 명확하게 설명한다
- 불필요하게 포괄적인 표현(`update`, `fix bug`, `misc`)은 피한다
- 마침표(`.`)는 사용하지 않는다
- 50~72자 이내를 권장한다

좋은 예시:

```text
fix(parser): 서브쿼리 끝 괄호 누락 시 에러 위치 보정 [DON-41]
refactor(proxy): 세션 종료 처리 로직을 연결 상태별로 분리 [DON-52]
```

나쁜 예시:

```text
fix(parser): 버그 수정 [DON-41]
chore(ci): update stuff [DON-30]
```

### Linear 이슈 ID 규칙 (필수)

- 커밋 메시지 끝에 반드시 `[DON-XX]` 형식으로 작성한다
- `XX`는 숫자이며, 실제 Linear 이슈 번호를 사용한다
- GitHub-Linear 자동 연동을 위해 형식을 정확히 지켜야 한다
- 권장 패턴: `\[DON-\d+\]`

### Breaking Change 표기

호환성 깨짐이 발생하는 경우 `type` 뒤에 `!`를 붙인다.

```text
feat(protocol)!: 인증 핸드셰이크 필드 포맷 변경 [DON-88]
```

필요하면 본문에 영향 범위와 마이그레이션 방법을 추가한다.

### 본문(body) 작성 (선택, 권장)

복잡한 변경은 제목 한 줄만으로 충분하지 않으므로 본문을 추가한다.

```text
fix(policy): YAML 파싱 시 빈 파일 크래시 수정 [DON-26]

빈 파일 입력에서 루트 노드 접근 전에 길이 검사를 추가한다.
기존 동작은 nil 접근으로 프로세스가 종료될 수 있었다.
```

권장 상황:

- 설계 의도/트레이드오프 설명이 필요한 경우
- 회귀(regression) 방지 목적의 변경인 경우
- 영향 범위가 넓거나 운영 리스크가 있는 경우

### 커밋 분리 원칙

- 서로 다른 목적의 변경을 한 커밋에 섞지 않는다
- 리팩터링과 버그 수정은 가능하면 분리한다
- 포맷팅-only 변경은 기능 변경 커밋과 분리한다
- 테스트는 해당 기능/수정 커밋에 함께 포함하거나, 이유가 있으면 별도 커밋으로 분리한다

---

## PR (Pull Request) 컨벤션

### PR 제목 형식

커밋 컨벤션과 동일한 형식을 사용한다. Squash merge 시 PR 제목이 main 커밋 메시지가 되므로 형식 통일이 필수다.

```text
<type>(scope): 설명 [DON-XX]
```

예시:

```text
feat(protocol): MySQL 프로토콜 패킷 파서 구현 [DON-24]
feat(project): 전체 모듈 인터페이스 헤더 파일 작성 [DON-23]
chore(ci): GitHub Actions CI 파이프라인 추가 [DON-30]
```

### PR 워크플로우

```text
1. feat/xxx 브랜치에서 작업
2. 커밋 (컨벤션 준수)
3. PR 생성 → 템플릿 자동 적용 (.github/pull_request_template.md)
4. Codex 자동 코드 리뷰 (@codex review)
5. CI 통과 확인
6. 리뷰 반영
7. main에 머지 (Squash merge 권장)
8. Linear 이슈 자동 Done
```

### PR 생성 규칙

- `gh pr create` 사용 시 `.github/pull_request_template.md` 내용을 본문에 포함할 것
- PR 본문 하단에 `@codex review`를 반드시 포함할 것
- 하나의 PR은 하나의 Linear 이슈에 대응한다
- 여러 이슈를 하나의 PR에 묶지 않는다 (Phase 1 초기 세팅 등 예외 허용)

### 머지 전 필수 조건

- [ ] CI 전체 통과 (빌드 + 테스트 + 정적 분석)
- [ ] Codex 리뷰 확인 (P0/P1 이슈 해결)
- [ ] clang-tidy 통과
- [ ] ASan/TSan 클린
- [ ] 단위 테스트 통과
- [ ] CLAUDE.md 규칙 준수

### 머지 전략

- **Squash merge** 권장: 피처 브랜치의 여러 커밋을 하나로 합쳐 main에 깔끔하게 반영
- Squash 시 머지 커밋 메시지는 PR 제목을 그대로 사용: `<type>(scope): 설명 [DON-XX]`
- commitlint 규칙과 동일하므로 추가 예외 처리 불필요
- 머지 후 피처 브랜치는 삭제한다

---

## 추후 권장 자동화

문서 규칙만으로는 일관성이 깨질 수 있으므로, 이후 아래 도입을 권장합니다.

- `commitlint`로 커밋 메시지 검사 (`[DON-XX]` 패턴 포함)
- `commit-msg` Git hook (예: Husky)
- CI에서 커밋 메시지 규칙 검증
- PR 제목 형식 검증 (`<type>\(.+\): .+ \[DON-\d+\]`)
