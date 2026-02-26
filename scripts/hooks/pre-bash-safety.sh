#!/bin/bash
# PreToolUse Hook: 위험한 Bash 명령 차단
# stdin으로 JSON 입력을 받아 tool_input.command를 검사한다.
# 위험 패턴 매칭 시 exit 2로 차단, stderr에 사유 출력.

set -euo pipefail

INPUT=$(cat)
COMMAND=$(echo "$INPUT" | jq -r '.tool_input.command // empty')

if [ -z "$COMMAND" ]; then
  exit 0
fi

# 위험 패턴 목록
if echo "$COMMAND" | grep -qE 'rm\s+-rf\s+(/|/workspace|\.git/?)\s*$'; then
  echo "차단: 프로젝트/git 저장소 삭제 명령이 감지되었습니다 — $COMMAND" >&2
  exit 2
fi

if echo "$COMMAND" | grep -qE 'git\s+reset\s+--hard'; then
  echo "차단: git reset --hard는 커밋 이력을 손상시킬 수 있습니다 — $COMMAND" >&2
  exit 2
fi

if echo "$COMMAND" | grep -qE 'git\s+clean\s+-f'; then
  echo "차단: git clean -f는 미추적 파일을 삭제합니다 — $COMMAND" >&2
  exit 2
fi

exit 0
