---
name: doc-impact
description: 현재 브랜치의 변경 파일을 분석하여 영향받는 문서를 자동 추천
allowed-tools: Bash, Read, Glob, Grep
---

# Doc Impact 분석

현재 브랜치에서 변경된 파일을 기반으로 업데이트가 필요한 문서를 분석한다.

## 변경 파일 목록
!`git diff --name-only main 2>/dev/null || echo "main 브랜치와 비교할 수 없습니다"`

## 분석 절차

1. `.claude/ownership-map.yaml` 파일을 읽는다.
2. 위 변경 파일 목록의 각 파일 경로를 ownership-map의 `path_rules`와 매칭한다.
3. 매칭된 규칙의 `doc_impact_candidates` 문서 목록을 수집한다.

## 출력 형식

각 영향받는 문서에 대해 수정 여부를 표시한다:

| 문서 | 상태 | 관련 규칙 |
|------|------|----------|
| docs/xxx.md | 수정됨 / 미수정 | <규칙명> |

## 조치 안내

- **미수정 문서가 있는 경우**: 해당 문서를 업데이트하거나, 예외 사유가 있으면 커밋에 아래 trailer를 추가하도록 안내한다.
  ```
  Docs-Impact: none
  Docs-Impact-Reason: <사유>
  ```
- **모든 필수 문서가 수정된 경우**: "Doc Impact 검사 통과" 메시지를 출력한다.
- **변경 파일이 없는 경우**: "변경 파일이 없습니다"를 출력한다.
