#!/bin/bash
# pre-bash-safety.sh 테스트 스크립트

SCRIPT="$(dirname "$0")/pre-bash-safety.sh"
PASS=0
FAIL=0

test_case() {
  local expected="$1"
  local cmd="$2"
  local result

  echo "{\"tool_input\":{\"command\":\"$cmd\"}}" | bash "$SCRIPT" 2>/dev/null
  local exit_code=$?

  if [ "$expected" = "block" ] && [ "$exit_code" -eq 2 ]; then
    echo "  PASS (차단됨): $cmd"
    ((PASS++))
  elif [ "$expected" = "pass" ] && [ "$exit_code" -eq 0 ]; then
    echo "  PASS (통과됨): $cmd"
    ((PASS++))
  else
    echo "  FAIL (expected=$expected, exit=$exit_code): $cmd"
    ((FAIL++))
  fi
}

echo "=== 차단되어야 하는 명령 ==="
test_case block "rm -rf /"
test_case block "rm -r -f /"
test_case block "rm --no-preserve-root -rf /"
test_case block "rm -rf -- /"
test_case block "rm -rf .git"
test_case block "rm -rf .git/"
test_case block "rm -fr /"
test_case block "rm -rf /."
test_case block "git reset --hard"
test_case block "git reset --hard HEAD~1"
test_case block "git clean -f"
test_case block "git clean -fd"
test_case block "git status && rm -rf /"
test_case block "git commit -m x; rm -rf /"
test_case block "git checkout foo && rm -rf .git"

echo ""
echo "=== 통과되어야 하는 명령 ==="
test_case pass "rm -rf build/"
test_case pass "rm temp.txt"
test_case pass "git status"
test_case pass "git add ."
test_case pass "cmake --build build/default"
test_case pass "git commit -m 'rm -rf .git 차단 테스트'"
test_case pass "git commit -m 'git status && rm -rf / 패턴 설명'"
test_case pass "git log --oneline -5"

echo ""
echo "결과: PASS=$PASS, FAIL=$FAIL"
