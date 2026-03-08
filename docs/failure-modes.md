# dbgate Failure Modes

## 목적
- 운영/CI에서 자주 발생하는 실패 모드를 빠르게 식별하고 재현 및 복구 절차를 제공한다.

## CI 실패 모드

### 1) Commit & PR Lint 실패
- 증상:
  - `PR 제목 형식 오류`
  - `커밋 메시지 형식 오류`
- 원인:
  - 제목/커밋이 `type(scope): 설명` 형식을 따르지 않음
  - 선택 항목인 `[DON-XX]`를 포함해도 되고 생략해도 됨
- 복구:
  - PR 제목 수정
  - 필요 시 커밋 메시지 정리 후 push

### 2) Docs Impact Check 실패
- 증상:
  - `FAIL: missing doc updates for required groups`
- 원인:
  - 코드 경로 변경 대비 문서 후보 파일 미갱신
- 복구:
  - 경로 그룹에 맞는 문서 1개 이상 갱신
  - 예외 시 커밋 트레일러 추가
    - `Docs-Impact: none`
    - `Docs-Impact-Reason: <specific reason>`

### 3) Build & Test에서 StructuredLogger 테스트 간헐 실패
- 증상:
  - `StructuredLoggerTest.*`에서 로그 파일 라인 수가 0으로 실패
- 원인:
  - `info` 레벨 로그 버퍼링으로 즉시 파일 반영이 보장되지 않음
- 복구:
  - `StructuredLogger` 종료 시 `flush()` 보장
  - 테스트는 로거 수명 종료 이후 파일을 읽도록 구성

## 런타임 실패 모드

### UDS 바인드 실패
- 증상:
  - `bind error on /tmp/...sock`
- 원인:
  - 파일 권한/경로 충돌/실행 환경 제한
- 복구:
  - 기존 소켓 파일 정리
  - 실행 사용자와 디렉터리 권한 확인
  - 컨테이너/샌드박스 정책 점검
