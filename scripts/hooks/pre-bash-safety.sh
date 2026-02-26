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

# 공백 우회 방지: $IFS/${IFS}를 실제 공백으로 정규화해 패턴 검사한다.
CHECK_COMMAND=$(echo "$COMMAND" | sed -E 's/\$\{IFS\}|\$IFS/ /g')

# 따옴표 내부를 제거한 문자열은 "안전한 단일 git 명령" 판별에만 사용한다.
# 보안 패턴 검사는 원본 COMMAND 기준으로 수행해 bash -c "rm -rf /" 같은 우회를 막는다.
STRIPPED_FOR_OPS=$(echo "$CHECK_COMMAND" | sed "s/'[^']*'//g; s/\"[^\"]*\"//g")

# 안전한 단일 git 명령은 조기 통과 (커밋 메시지 내 텍스트 오탐 방지)
if echo "$STRIPPED_FOR_OPS" | grep -qE '^[[:space:]]*git[[:space:]]+(commit|log|show|diff|status|add|push|pull|fetch|branch|checkout|merge|rebase|stash|tag|remote)\b' \
  && ! echo "$STRIPPED_FOR_OPS" | grep -qE '&&|\|\||;|\|' \
  && [[ "$COMMAND" != *$'\n'* ]] \
  && [[ "$COMMAND" != *'$('* ]] \
  && [[ "$COMMAND" != *'`'* ]]; then
  exit 0
fi

# rm 계열: 위험 경로(/, .git)를 대상으로 하는 rm 명령은 플래그 무관하게 차단
# (따옴표/개행/체이닝 포함 입력도 탐지)
if echo "$CHECK_COMMAND" | grep -qE '(^|[^[:alnum:]_])rm([[:space:]]|$)' \
  && echo "$CHECK_COMMAND" | grep -qE '(^|[[:space:]])/($|[^[:alnum:]_-])|(^|[[:space:]])\.git($|[^[:alnum:]_-])'; then
  echo "차단: 위험 경로(/ 또는 .git)에 대한 rm 명령이 감지되었습니다 — $COMMAND" >&2
  exit 2
fi

# git reset --hard: 옵션 순서 무관하게 차단
if echo "$CHECK_COMMAND" | grep -qE '(^|[^[:alnum:]_])git([[:space:]]|$)' \
  && echo "$CHECK_COMMAND" | grep -qE '(^|[^[:alnum:]_])reset([[:space:]]|$)' \
  && echo "$CHECK_COMMAND" | grep -q -- '--hard'; then
  echo "차단: git reset --hard는 커밋 이력을 손상시킬 수 있습니다 — $COMMAND" >&2
  exit 2
fi

# git clean -f: -fd, -fx 등 변형 포함
if echo "$CHECK_COMMAND" | grep -qE '(^|[^[:alnum:]_])git([[:space:]]|$)' \
  && echo "$CHECK_COMMAND" | grep -qE '(^|[^[:alnum:]_])clean([[:space:]]|$)' \
  && echo "$CHECK_COMMAND" | grep -qE '(^|[[:space:]])-[[:alnum:]-]*f[[:alnum:]-]*($|[^[:alnum:]_-])'; then
  echo "차단: git clean -f는 미추적 파일을 삭제합니다 — $COMMAND" >&2
  exit 2
fi

exit 0
