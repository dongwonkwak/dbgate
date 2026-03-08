# 테스트 데이터 가이드라인

AI 도구(Claude Code, Codex CLI 등)와 사람 모두가 따르는 테스트 데이터 파일 관리 규칙.

## 네이밍 규칙

**형식**: `[카테고리]_[설명].[확장자]`

- 소문자 + 언더스코어만 사용 (`[a-z0-9_]+\.[a-z]+`)
- 파일명만으로 테스트 목적이 유추 가능해야 함

**예시**:
```
select_simple.sql       # 기본 SELECT 파싱 검증
com_query_select1.bin   # COM_QUERY 패킷 파싱 검증
sqli_union.sql          # UNION 기반 SQL 인젝션 탐지
empty_payload.bin       # 빈 페이로드 경계값 처리
```

## 금지 패턴

다음 패턴의 파일명은 pre-commit 훅에서 차단된다:

| 패턴 | 예시 | 사유 |
|------|------|------|
| 해시명 (`^[0-9a-f]{32,}`) | `a1b2c3d4e5f6...` | AI 자동생성 퍼즈 출력물 |
| 임시 접두사 (`^temp_`, `^tmp_`) | `temp_test.sql` | 의도 불명확 |
| 미명명 (`^untitled`) | `untitled1.bin` | 의도 불명확 |
| 대문자 포함 | `Select_Simple.sql` | 일관성 위반 |
| 공백 포함 | `select simple.sql` | 경로 처리 문제 |

## DATA_CATALOG.yaml

각 테스트 데이터 디렉토리에 `DATA_CATALOG.yaml`을 유지한다.

### 구조

```yaml
catalog_version: 1
description: "이 디렉토리의 테스트 데이터 설명"
files:
  파일명.확장자:
    intent: "이 파일이 무엇을 테스트하는지 한 줄 설명"
    format: "바이너리 파일의 바이트 레이아웃 설명 (선택)"
```

### 필드

| 필드 | 필수 | 설명 |
|------|------|------|
| `intent` | O | 파일의 테스트 목적 (빈 문자열 금지) |
| `format` | 바이너리 파일 권장 | 파일 형식 설명 (텍스트 파일은 내용으로 유추 가능하므로 선택) |

### 동기화 규칙

- 파일 추가 시: `DATA_CATALOG.yaml`에 엔트리 추가 필수
- 파일 삭제 시: `DATA_CATALOG.yaml`에서 엔트리 제거 필수
- `DATA_CATALOG.yaml`에 엔트리가 있는데 실제 파일이 없으면 커밋 차단

## 검증 대상 경로

현재 검증 대상: `tests/fuzz/corpus/**`

향후 테스트 데이터 디렉토리가 추가되면 `scripts/check-test-data.sh`의 `CORPUS_ROOT` 배열에 경로를 추가한다.

## 예외 처리

검증을 건너뛰어야 하는 경우, 커밋 메시지에 트레일러를 추가한다:

```
Test-Data-Doc: skip
Test-Data-Doc-Reason: <사유>
```

남용 금지: 단순히 카탈로그 작성이 귀찮다는 이유로 예외를 사용하지 않는다.

## 개발 워크플로우

1. **개발 중**: 자유롭게 테스트 데이터 생성/수정
2. **커밋 시**: pre-commit 훅이 네이밍 + 카탈로그 동기화 자동 검증
3. **PR 시**: CI가 동일 검증 재실행
