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

# 커밋 메시지 등 따옴표/heredoc 안의 텍스트를 제거하여 오탐 방지
# 실제 명령어 부분만 남긴 뒤 위험 패턴을 검사한다
STRIPPED=$(echo "$COMMAND" | sed "s/'[^']*'//g; s/\"[^\"]*\"//g")

# rm 계열: 위험 경로(/, .git)를 대상으로 하는 rm 명령은 플래그 무관하게 차단
if echo "$STRIPPED" | grep -qE '\brm\s' && echo "$STRIPPED" | grep -qE '(^|\s)(\/([^a-zA-Z0-9_-]|$)|\.git)'; then
  echo "차단: 위험 경로(/ 또는 .git)에 대한 rm 명령이 감지되었습니다 — $COMMAND" >&2
  exit 2
fi

# git reset --hard: 옵션 순서 무관하게 차단
if echo "$STRIPPED" | grep -qE 'git\s+reset\s+.*--hard|git\s+.*--hard\s+.*reset'; then
  echo "차단: git reset --hard는 커밋 이력을 손상시킬 수 있습니다 — $COMMAND" >&2
  exit 2
fi

# git clean -f: -fd, -fx 등 변형 포함
if echo "$STRIPPED" | grep -qE 'git\s+clean\s+-[a-zA-Z]*f'; then
  echo "차단: git clean -f는 미추적 파일을 삭제합니다 — $COMMAND" >&2
  exit 2
fi

exit 0
