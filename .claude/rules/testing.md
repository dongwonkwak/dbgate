---
paths:
  - "tests/**"
  - "benchmarks/**"
---

# 테스트 규칙

## 필수 사항
- 모든 public 함수에 단위 테스트
- 정상 경로 + 에러 경로 + 경계값 필수

## 네이밍
- 테스트 이름: `Test_대상_상황_기대결과` 형태

## 검증 기준
- ASan/TSan 빌드에서 모든 테스트 클린
- 실패 메시지로 원인을 추적할 수 있어야 함
- flaky 테스트 허용하지 않음

## 빌드 명령어
- 기본 테스트: `cmake --build build/default --target test`
- ASan: `cmake --preset asan && cmake --build build/asan`
- TSan: `cmake --preset tsan && cmake --build build/tsan`

## 기술 스택
- Google Test (C++ 단위 테스트)
- libFuzzer (퍼징)
- AddressSanitizer, ThreadSanitizer
- sysbench (벤치마크)
