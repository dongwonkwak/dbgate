---
name: build-test
description: C++ 프로젝트 빌드, 테스트, ASan 메모리 검사를 순차적으로 수행
allowed-tools: Bash
---

# 빌드 및 테스트 실행

아래 단계를 순서대로 수행하고, 각 단계의 성공/실패를 보고한다.
실패한 단계가 있으면 즉시 멈추고 에러 내용을 분석하여 보고한다.

## 1단계: 기본 빌드
```bash
cmake --preset default && cmake --build build/default
```

## 2단계: 테스트 실행
```bash
cmake --build build/default --target test
```
실패 시 개별 테스트 바이너리를 직접 실행하여 상세 출력을 확인한다.

## 3단계: ASan 빌드 및 테스트
```bash
cmake --preset asan && cmake --build build/asan
cmake --build build/asan --target test
```

## 결과 보고
모든 단계 완료 후 아래 형식으로 요약한다:

| 단계 | 결과 |
|------|------|
| 기본 빌드 | ? |
| 테스트 | ? |
| ASan 빌드 | ? |
| ASan 테스트 | ? |
