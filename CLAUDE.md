# CLAUDE.md

## 프로젝트 개요
DB 접근제어 프록시. C++ 코어 + Go 운영도구.
MySQL 클라이언트와 서버 사이에 위치하여 SQL을 파싱하고 정책 기반으로 차단/허용/로깅을 수행한다.

## 아키텍처 규칙
- C++ 코어는 src/ 하위, Go 도구는 tools/ 하위
- C++ ↔ Go 통신은 반드시 Unix Domain Socket
- 새 파일 추가 시 CMakeLists.txt 업데이트 필수

## C++ 코딩 규칙
- C++20, GCC 14 기준
- 비동기는 반드시 Boost.Asio co_await 사용 (raw thread 금지)
- 메모리: shared_ptr/unique_ptr 사용, raw new/delete 금지
- 에러: 예외 대신 expected/error_code 패턴
- 로깅: spdlog 사용, fmt::format 스타일
- 네이밍: snake_case (함수/변수), PascalCase (클래스/구조체)

## Go 코딩 규칙
- 표준 Go 컨벤션, golangci-lint 통과 필수
- 에러는 반드시 처리 (_ 무시 금지)

## 테스트
- 모든 public 함수에 단위 테스트
- C++ 테스트: cmake --build build --target test
- Go 테스트: cd tools && go test -race ./...

## 브랜치 규칙
- main은 항상 빌드 + 테스트 통과 상태
- 브랜치명: feat/모듈명, fix/이슈명
- 커밋 메시지에 Linear 이슈 ID (DON-XX) 포함
- 머지 전 반드시: clang-tidy 통과, ASan/TSan 클린, 테스트 통과

## 빌드 명령어
- 기본 빌드: cmake --preset default && cmake --build build/default
- 디버그 빌드: cmake --preset debug && cmake --build build/debug
- ASan 빌드: cmake --preset asan && cmake --build build/asan
- TSan 빌드: cmake --preset tsan && cmake --build build/tsan

## 절대 하지 말 것
- raw epoll 직접 사용
- 전역 변수
- using namespace std;
- 하드코딩된 포트/경로 (config에서 읽을 것)
