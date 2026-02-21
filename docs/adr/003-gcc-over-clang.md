# ADR-003: GCC 14 over Clang

## Status

Accepted

## Context

C++20 코루틴(`co_await`)을 핵심적으로 사용하는 dbgate 프로젝트에서 기본 컴파일러를 선택해야 한다. 주요 후보는 GCC 14와 Clang 18이다.

## Decision

**GCC 14**를 기본 컴파일러로 사용한다. 단, Clang 호환성은 유지한다.

## Consequences

### Positive

- **코루틴 안정성**: GCC 14의 C++20 코루틴 구현이 Boost.Asio와의 조합에서 안정적으로 동작. Clang은 일부 코루틴 edge case에서 codegen 이슈가 보고된 바 있음.
- **Linux 표준 컴파일러**: Ubuntu 24.04의 기본 C++ 컴파일러로 별도 설치 없이 사용 가능.
- **libstdc++ 호환성**: Boost, spdlog 등 대부분의 C++ 라이브러리가 libstdc++를 기준으로 테스트됨.
- **Sanitizer 지원**: ASan, TSan, UBSan 모두 GCC 14에서 안정적으로 동작.

### Negative

- **Clang 도구 의존**: clang-tidy, clang-format은 Clang 생태계 도구이므로 별도 설치 필요 (빌드와 분석 도구 분리).
- **일부 진단 메시지**: Clang의 에러/경고 메시지가 더 읽기 쉬운 경우가 있음.

### Alternatives Considered

- **Clang 18**: 뛰어난 에러 메시지, libc++ 사용 시 모듈 지원 등 장점이 있으나, Boost.Asio 코루틴과의 조합에서 간헐적 이슈 보고. libc++ vs libstdc++ 선택도 추가 복잡도를 유발.

### Migration Path

Clang 호환성을 유지하므로, 향후 Clang의 코루틴 지원이 완전히 안정화되면 전환 가능.
