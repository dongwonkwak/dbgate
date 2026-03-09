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

### 4) Static Analysis (clang-tidy) 실패
- 증상:
  - `cppcoreguidelines-avoid-const-or-ref-data-members`
  - `readability-braces-around-statements`
  - `misc-const-correctness`
  - `modernize-return-braced-init-list`
  - Boost.Asio `awaitable.hpp` 경유 `clang-analyzer-core.NullDereference`
- 원인:
  - 세션 내부 보조 버퍼 클래스가 reference data member를 유지함
  - coroutine early return 분기에 brace가 빠져 style check에 걸림
  - 변경되지 않는 scatter-gather 전송 버퍼가 `const`로 선언되지 않음
  - 단순 문자열 반환식이 최신 초기화 스타일과 맞지 않음
  - clang static analyzer가 coroutine + Boost.Asio awaitable 경로를 추적하며 외부 헤더 false positive를 낼 수 있음
- 복구:
  - non-owning stream 참조는 pointer 등 재바인딩 가능한 형태로 저장
  - 단일문 분기에도 brace를 사용
  - 불변 로컬 버퍼는 `const`로 선언
  - 단순 생성 반환은 braced-init-list 사용
  - `clang-tidy -p build/default <file>.cpp` 단독 실행 대신 CI와 같은 extra arg/별도 compile DB 구성으로 재현한다
  - 외부 Boost 헤더에서만 발생한 `clang-analyzer-core.NullDereference`는 `.clang-tidy`에서 `WarningsAsErrors` 제외 항목이므로, 프로젝트 소스 error가 없으면 CI 실패 원인으로 보지 않는다

### 5) devcontainer에서 Integration Test 확인이 불안정한 경우
- 증상:
  - `tests/integration/test_scenarios.sh` 실행 시 direct/proxy 단계가 모두 실패하거나 환경에 따라 결과가 달라짐
- 원인:
  - devcontainer 내부에서는 Docker daemon 제어가 불가하고, compose MySQL 접근도 실행 권한/네트워크 제약의 영향을 받음
  - 스크립트는 stderr를 버리므로 접속 실패 원인이 바로 드러나지 않음
- 복구:
  - compose가 올린 `mysql` 서비스에 직접 접속 가능한 실행 경로에서 검증
  - 필요 시 동일한 `mysql` 명령을 독립 실행해 실제 접속 오류를 먼저 확인

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
