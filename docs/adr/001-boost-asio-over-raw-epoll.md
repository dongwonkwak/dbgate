# ADR-001: Boost.Asio over Raw epoll

## Status

Accepted

## Context

dbgate는 MySQL 클라이언트와 서버 사이에 위치하는 투명 프록시로, 대량의 동시 연결을 비동기로 처리해야 한다. Linux 환경에서 비동기 I/O를 구현하는 방법은 크게 두 가지이다:

1. **Raw epoll**: Linux 시스템 콜을 직접 사용
2. **Boost.Asio**: 추상화된 비동기 I/O 라이브러리 (내부적으로 epoll 사용)

## Decision

**Boost.Asio**를 비동기 I/O 프레임워크로 사용한다.

## Consequences

### Positive

- **C++20 코루틴 지원**: `co_await`를 활용한 깔끔한 비동기 코드 작성 가능. 콜백 지옥 회피.
- **strand를 통한 스레드 안전성**: 멀티스레드 환경에서 명시적 뮤텍스 없이 동시성 제어 가능.
- **SSL/TLS 내장**: `boost::asio::ssl` 으로 클라이언트-프록시, 프록시-MySQL 양 구간 암호화를 일관된 API로 구현.
- **타이머, 시그널 핸들링 통합**: Graceful Shutdown(SIGTERM), 정책 Hot Reload(SIGHUP), Health Check 타이머 등을 동일한 이벤트 루프에서 처리.
- **크로스 플랫폼 호환**: 프로덕션은 Linux(epoll)이지만 macOS(kqueue)에서도 빌드/디버깅 가능.
- **검증된 안정성**: 수많은 프로덕션 환경에서 사용된 성숙한 라이브러리.

### Negative

- **Boost 의존성 추가**: 빌드 시간 증가, vcpkg로 관리 필요.
- **추상화 오버헤드**: raw epoll 대비 미미한 성능 오버헤드 존재 (DB 프록시 워크로드에서는 무시 가능한 수준).
- **디버깅 복잡도**: Boost 내부 스택 트레이스가 깊어 디버깅이 다소 어려움.

### Alternatives Considered

- **Raw epoll**: 최대 성능을 확보할 수 있으나, SSL, 타이머, 시그널 핸들링을 모두 직접 구현해야 하며 코루틴 통합도 수동. 핵심 비즈니스 로직(SQL 파싱, 정책 엔진)에 집중하기 위해 채택하지 않음.
- **libuv**: Node.js 생태계에서 검증되었으나 C++ 코루틴 통합이 불편하고 C API라 타입 안전성이 낮음.
- **io_uring**: 최신 Linux 커널에서 높은 성능을 제공하지만 아직 Boost.Asio 수준의 생태계/문서가 부족하고 커널 버전 요구사항이 높음.
