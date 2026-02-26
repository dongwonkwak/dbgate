---
name: start-task
description: Linear 이슈 기반으로 브랜치 생성 및 작업 환경을 자동 준비
argument-hint: DON-XX
allowed-tools: Bash, Read, Glob, Grep, mcp__*
---

# 작업 환경 준비

Linear 이슈 `$ARGUMENTS`를 기반으로 작업 환경을 자동 준비한다.

## 1단계: Linear 이슈 조회
Linear MCP `get_issue`로 `$ARGUMENTS` 이슈를 조회한다.
- 이슈 제목, 설명, 상태, 라벨을 확인한다.

## 2단계: 브랜치명 결정
이슈 제목에서 커밋 타입을 추출하여 브랜치명을 생성한다.
- `feat(...)` → `feat/$ARGUMENTS-<모듈명>`
- `fix(...)` → `fix/$ARGUMENTS-<설명>`
- `chore(...)` → `chore/$ARGUMENTS-<설명>`
- 기타 → `feat/$ARGUMENTS-<요약>`

브랜치명은 영문 소문자, 하이픈만 사용한다.

## 3단계: 브랜치 생성
```bash
git checkout -b <브랜치명>
```
이미 해당 브랜치가 존재하면 checkout만 수행한다.

## 4단계: Linear 상태 변경
Linear MCP `save_issue`로 이슈 상태를 `In Progress`로 변경한다.

## 5단계: Brief 확인
이슈 코멘트 목록(`list_comments`)에서 Execution Brief가 있는지 확인한다.
없으면 경고를 출력한다:
> "Execution Brief가 아직 작성되지 않았습니다. `docs/process/execution-brief-template.md`를 참고하여 작성해주세요."

## 완료 보고
준비된 환경을 요약하여 보고한다:
- 이슈: `$ARGUMENTS` — <제목>
- 브랜치: `<브랜치명>`
- 상태: In Progress
- Brief: 있음/없음
