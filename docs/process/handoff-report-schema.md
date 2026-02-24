# 서브에이전트 완료 보고 스키마 (Handoff Report Schema)

## 목적
- 모든 서브에이전트가 같은 구조로 결과를 보고하게 하여, 호출자/리뷰어/다른 AI가 빠르게 판단할 수 있게 한다.
- 문서 영향, 인터페이스 영향, 운영 영향을 누락 없이 보고하게 한다.

## 적용 대상
- `network-engineer`
- `security-engineer`
- `infra-engineer`
- `go-engineer`
- `qa-engineer`
- `technical-writer` (문서 전용 변형 포함)

## 공통 필드 (필수)

### 1) 변경 파일 목록
- 실제 수정 파일 경로 나열

### 2) 변경 요약
- 무엇이 바뀌었는지 요약
- 역할별 특화 표현 허용 (예: `파서/탐지/정책`, `CLI/대시보드`, `문서별 핵심 내용`)

### 3) 변경 분류
- 아래 중 하나 이상 선택:
  - `behavior`
  - `interface`
  - `ops`
  - `perf`
  - `docs-only`
  - `internal-refactor`

`technical-writer`는 문서 성격에 맞게 변형 사용 가능:
- `behavior-doc`
- `interface-doc`
- `ops-doc`
- `adr`
- `readme`
- `internal-doc`

### 4) 인터페이스 영향
- 있음/없음
- 영향 대상(헤더/함수/UDS 커맨드/JSON 필드/CLI 명령 등) 명시

### 5) 운영 영향
- 있음/없음
- 설정/로그/헬스체크/배포/롤백/운영 절차 영향 명시

### 6) 문서 영향 분석 (필수)
아래 하위 필드를 반드시 포함:
- `변경 동작/인터페이스/운영 영향`
- `영향 문서 후보`
- `실제 수정 문서`
- `문서 미수정 사유(해당 시)`

### 7) 테스트/검증 결과
- 빌드/테스트/린트/재현 절차/벤치마크 등 역할에 맞는 결과

### 8) 교차영향 및 후속 요청
- 다른 에이전트/Architect/QA/writer에 필요한 후속 작업 또는 확인 사항

## 변경 분류 기준 (간단 버전)

### `behavior`
- 외부 관찰 가능한 동작 변화 (허용/차단/응답/에러 메시지/흐름)

### `interface`
- 함수 시그니처, 헤더 계약, UDS 커맨드/필드, CLI 명령 옵션/출력 계약 변화

### `ops`
- 배포/설정/헬스체크/로그/운영 절차/CI 변화

### `perf`
- 핫패스 최적화, 메모리 할당/복사 감소, 지연시간/처리량 영향

### `docs-only`
- 코드 동작 없이 문서만 변경

### `internal-refactor`
- 동작/인터페이스/운영 영향 없이 내부 구조만 정리

## 예시 (구현 에이전트)

```md
- 변경 파일 목록
  - src/proxy/session.cpp
  - tests/test_proxy_session.cpp
  - docs/data-flow.md
- 변경 요약 (구현/동작 변화)
  - COM_QUERY 처리 중 malformed packet 에러 경로에서 세션 종료 및 로그 필드 보강
- 변경 분류
  - behavior
  - ops
- 인터페이스 영향
  - 없음
- 운영 영향
  - 있음 (에러 로그 필드 추가)
- 문서 영향 분석
  - 변경 동작/인터페이스/운영 영향: malformed packet 처리 흐름/로그 항목 변경
  - 영향 문서 후보: docs/data-flow.md, docs/observability.md
  - 실제 수정 문서: docs/data-flow.md
  - 문서 미수정 사유(해당 시): observability 문서는 로그 스키마가 아직 확정 전
- 테스트 추가/수정 내용
  - malformed packet 단위 테스트 추가
- 빌드/테스트 실행 결과
  - 관련 테스트 통과
- 교차영향 및 후속 요청
  - infra-engineer: 로그 스키마 확정 시 observability 문서 반영 필요
```

## 예시 (문서 에이전트)

```md
- 변경 문서 목록
  - docs/architecture.md
  - docs/data-flow.md
- 변경 요약 (문서별 핵심 내용)
  - COM_QUERY 에러 경로와 fail-close 동작 설명 보완
- 변경 분류
  - behavior-doc
- 인터페이스/동작 문서화 영향
  - 파싱 실패 시 차단/종료 흐름을 명시
- 운영 문서 영향
  - 없음
- 문서 영향 분석
  - 변경 동작/인터페이스/운영 영향: 기존 구현 반영
  - 영향 문서 후보: docs/architecture.md, docs/data-flow.md
  - 실제 수정 문서: docs/architecture.md, docs/data-flow.md
  - 문서 미수정 사유(해당 시):
- 코드/설정과 교차검증한 항목
  - src/proxy/session.cpp 에러 처리 경로
- 남은 빈칸/추가 확인 필요 항목
  - observability 로그 필드명 확정 필요
- 교차영향 및 후속 요청
  - infra-engineer/Architect 확인 요청
```

## AI 사용 규칙 (요약)
1. 완료 보고는 자유 서술이 아니라 위 필드 순서를 따른다.
2. `문서 영향 분석`은 생략하지 않는다.
3. `internal-refactor`라도 문서 미수정 사유를 적는다.
