# 테스트 데이터 가이드라인

퍼징 테스트 데이터는 `seeds`(수동 관리)와 `generated`(자동 생성)로 분리한다.

## 디렉토리 정책

- `tests/fuzz/seeds/**`
  - 사람이 관리하는 시드 코퍼스
  - Git 추적 대상
- `tests/fuzz/generated/**`
  - libFuzzer 실행 중 자동 생성되는 코퍼스
  - Git 비추적 대상 (`.gitignore`)

## seeds 네이밍 규칙

**형식**: `[category]_[description].[ext]`

- 소문자 + 언더스코어 사용 (`[a-z0-9_]+\.[a-z]+`)
- 파일명만 보고 테스트 의도를 파악할 수 있어야 함

예시:

```text
select_simple.sql
com_query_select1.bin
sqli_union.sql
empty_payload.bin
```

## seeds 금지 패턴

- 해시명 (`^[0-9a-f]{32,}$`)
- 임시 접두사 (`temp_`, `tmp_`)
- 미명명 (`untitled*`)
- 대문자/공백 포함 파일명

## DATA_CATALOG.yaml 운영

- `tests/fuzz/seeds/*/DATA_CATALOG.yaml` 유지 권장
- 시드 파일 추가/삭제 시 카탈로그 동기화 권장
- 자동 강제 훅은 두지 않으며, 리뷰/PR에서 의도를 확인한다

## 개발 워크플로우

1. 수동 시드는 `tests/fuzz/seeds/`에 추가/수정
2. 퍼저 실행 시 출력 코퍼스는 `tests/fuzz/generated/`(또는 `/tmp`)로 지정
3. `generated` 산출물은 커밋하지 않는다
4. pre-commit 훅은 `generated` staged 파일을 거부하고, `seeds`의 untracked 해시 파일을 자동 정리한다
