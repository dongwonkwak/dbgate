---
paths:
  - "tools/**/*.go"
  - "tools/go.mod"
  - "tools/go.sum"
---

# Go 코딩 규칙

## 컨벤션
- 표준 Go 컨벤션 준수
- `golangci-lint` 통과 필수

## 에러 처리
- 에러는 반드시 처리 (`_` 무시 금지)

## 통신
- UDS 프로토콜/JSON 필드/command 변경 시 architect 승인 필수

## 테스트
- `cd tools && go test -race ./...`
