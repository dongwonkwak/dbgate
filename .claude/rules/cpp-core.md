---
paths:
  - "src/**/*.cpp"
  - "src/**/*.hpp"
  - "CMakeLists.txt"
---

# C++ 코어 코딩 규칙

## 언어 및 빌드
- C++23 표준, GCC 14 기준
- 새 파일 추가 시 `CMakeLists.txt` 반드시 업데이트

## 비동기
- 반드시 Boost.Asio `co_await` 사용
- raw thread, raw epoll 금지
- `strand`로 스레드 안전성 확보

## 메모리
- `shared_ptr`/`unique_ptr` 사용
- raw `new`/`delete` 금지

## 에러 처리
- 예외 대신 `std::expected<T, E>` 패턴
- `boost::system::error_code` 허용

## 로깅
- `spdlog` 사용, `fmt::format` 스타일

## 네이밍
- `snake_case`: 함수, 변수
- `PascalCase`: 클래스, 구조체

## 금지사항
- `using namespace std;`
- 전역 변수
- 하드코딩된 포트/경로 (config에서 읽을 것)
