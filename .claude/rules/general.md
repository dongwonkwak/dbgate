# PR 생성

PR을 만들기 전에 **반드시** `.github/pull_request_template.md`를 먼저 읽고 템플릿 형식을 따를 것.

# 빌드 명령어

## C++ 빌드
- 기본 빌드: `cmake --preset default && cmake --build build/default`
- 디버그 빌드: `cmake --preset debug && cmake --build build/debug`

## Go 빌드
- `cd tools && go build ./...`
