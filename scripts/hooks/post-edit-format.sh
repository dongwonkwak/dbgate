#!/bin/bash
# PostToolUse Hook: C++ 파일 편집 후 clang-format 자동 실행
# clang-format이 설치되지 않은 환경에서는 조용히 skip한다.

set -euo pipefail

INPUT=$(cat)
FILE_PATH=$(echo "$INPUT" | jq -r '.tool_input.file_path // empty')

if [ -z "$FILE_PATH" ]; then
  exit 0
fi

# C++ 파일만 대상
case "$FILE_PATH" in
  *.cpp|*.hpp)
    ;;
  *)
    exit 0
    ;;
esac

# clang-format 존재 여부 확인 (없으면 skip)
if ! command -v clang-format &>/dev/null; then
  exit 0
fi

# 파일 존재 확인
if [ ! -f "$FILE_PATH" ]; then
  exit 0
fi

clang-format -i "$FILE_PATH"
exit 0
