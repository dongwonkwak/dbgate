#!/usr/bin/env bash
# check-test-data.sh — 테스트 데이터 네이밍 + 카탈로그 동기화 검증
# exit 0=pass, 1=fail, 2=error
set -euo pipefail

# 검증 대상 루트 디렉토리 (추가 시 여기에 경로 추가)
CORPUS_ROOTS=("tests/fuzz/corpus")
CATALOG_FILE="DATA_CATALOG.yaml"

ERRORS=()

# --------------------------------------------------------------------------
# 유틸: staged 파일 목록 (CI 모드에서는 --base/--head 사용)
# --------------------------------------------------------------------------
MODE="precommit"
BASE_REF=""
HEAD_REF=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base) BASE_REF="$2"; MODE="ci"; shift 2 ;;
    --head) HEAD_REF="$2"; MODE="ci"; shift 2 ;;
    *) echo "[test-data] 알 수 없는 옵션: $1" >&2; exit 2 ;;
  esac
done

get_changed_files() {
  if [[ "$MODE" == "ci" ]]; then
    git diff --name-only --diff-filter=ACMRD "$BASE_REF"..."$HEAD_REF" 2>/dev/null || true
  else
    git diff --cached --name-only --diff-filter=ACMRD 2>/dev/null || true
  fi
}

get_deleted_files() {
  if [[ "$MODE" == "ci" ]]; then
    git diff --name-only --diff-filter=D "$BASE_REF"..."$HEAD_REF" 2>/dev/null || true
  else
    git diff --cached --name-only --diff-filter=D 2>/dev/null || true
  fi
}

# --------------------------------------------------------------------------
# 예외 트레일러 검사
# --------------------------------------------------------------------------
check_skip_trailer() {
  local msg
  if [[ "$MODE" == "ci" ]]; then
    msg=$(git log -1 --format='%B' "$HEAD_REF" 2>/dev/null || true)
  else
    # pre-commit: 아직 커밋 전이므로 .git/COMMIT_EDITMSG 사용
    local commit_msg_file="${GIT_DIR:-.git}/COMMIT_EDITMSG"
    if [[ -f "$commit_msg_file" ]]; then
      msg=$(cat "$commit_msg_file")
    else
      msg=""
    fi
  fi

  if echo "$msg" | grep -qE '^Test-Data-Doc:\s*skip\s*$'; then
    return 0
  fi
  return 1
}

# --------------------------------------------------------------------------
# 코퍼스 파일 필터링
# --------------------------------------------------------------------------
CHANGED_FILES=$(get_changed_files)
DELETED_FILES=$(get_deleted_files)

CORPUS_FILES=()
CORPUS_DIRS_TOUCHED=()

for root in "${CORPUS_ROOTS[@]}"; do
  while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    if [[ "$f" == "$root"/* ]]; then
      CORPUS_FILES+=("$f")
      # 변경된 파일의 디렉토리 추출
      dir=$(dirname "$f")
      # DATA_CATALOG.yaml 자체의 변경은 디렉토리로 포함
      if [[ "$(basename "$f")" == "$CATALOG_FILE" ]]; then
        dir="$dir"
      fi
      # 중복 제거하며 디렉토리 목록 추가
      local_found=0
      for d in "${CORPUS_DIRS_TOUCHED[@]+"${CORPUS_DIRS_TOUCHED[@]}"}"; do
        [[ "$d" == "$dir" ]] && local_found=1 && break
      done
      [[ $local_found -eq 0 ]] && CORPUS_DIRS_TOUCHED+=("$dir")
    fi
  done <<< "$CHANGED_FILES"

  # 삭제된 파일도 체크
  while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    if [[ "$f" == "$root"/* ]]; then
      dir=$(dirname "$f")
      local_found=0
      for d in "${CORPUS_DIRS_TOUCHED[@]+"${CORPUS_DIRS_TOUCHED[@]}"}"; do
        [[ "$d" == "$dir" ]] && local_found=1 && break
      done
      [[ $local_found -eq 0 ]] && CORPUS_DIRS_TOUCHED+=("$dir")
    fi
  done <<< "$DELETED_FILES"
done

# 코퍼스 변경 없으면 즉시 pass
if [[ ${#CORPUS_FILES[@]} -eq 0 && ${#CORPUS_DIRS_TOUCHED[@]} -eq 0 ]]; then
  exit 0
fi

echo "[test-data] 코퍼스 변경 감지, 검증 시작..."

# 예외 트레일러 확인
if check_skip_trailer; then
  echo "[test-data] Test-Data-Doc: skip 트레일러 감지 — 검증 생략"
  exit 0
fi

# --------------------------------------------------------------------------
# 1. 네이밍 검증
# --------------------------------------------------------------------------
NAMING_ERRORS=()

for f in "${CORPUS_FILES[@]}"; do
  basename_f=$(basename "$f")

  # DATA_CATALOG.yaml 자체는 네이밍 검증 제외
  [[ "$basename_f" == "$CATALOG_FILE" ]] && continue

  # 해시명 검사: 확장자 제거 후 체크
  name_without_ext="${basename_f%.*}"
  if echo "$name_without_ext" | grep -qE '^[0-9a-f]{32,}$'; then
    NAMING_ERRORS+=("$f: 해시명 금지 (의미 있는 이름 사용)")
  fi

  # temp_/tmp_/untitled 접두사 검사
  if echo "$basename_f" | grep -qiE '^(temp_|tmp_|untitled)'; then
    NAMING_ERRORS+=("$f: 임시 파일명 금지 (temp_/tmp_/untitled)")
  fi

  # 대문자 검사
  if echo "$basename_f" | grep -qE '[A-Z]'; then
    NAMING_ERRORS+=("$f: 대문자 금지 (소문자만 사용)")
  fi

  # 공백 검사
  if echo "$basename_f" | grep -qE ' '; then
    NAMING_ERRORS+=("$f: 공백 금지 (언더스코어 사용)")
  fi
done

if [[ ${#NAMING_ERRORS[@]} -gt 0 ]]; then
  for err in "${NAMING_ERRORS[@]}"; do
    ERRORS+=("[네이밍] $err")
  done
fi

# --------------------------------------------------------------------------
# 2. 카탈로그 동기화 검증
# --------------------------------------------------------------------------

# 순수 bash YAML 파서 (DATA_CATALOG.yaml에서 파일명 추출)
# files: 섹션 아래의 "  파일명:" 패턴을 추출
extract_catalog_entries() {
  local catalog_path="$1"
  [[ ! -f "$catalog_path" ]] && return
  local in_files_section=0
  while IFS= read -r line; do
    # files: 섹션 시작
    if echo "$line" | grep -qE '^files:'; then
      in_files_section=1
      continue
    fi
    # files 섹션 내 최상위 키 (파일명)
    if [[ $in_files_section -eq 1 ]]; then
      # 들여쓰기 없는 새 섹션이면 종료
      if echo "$line" | grep -qE '^[a-zA-Z]'; then
        break
      fi
      # "  파일명:" 패턴 (2칸 들여쓰기)
      if echo "$line" | grep -qE '^  [a-zA-Z0-9_][a-zA-Z0-9_./-]*:'; then
        local entry
        entry=$(echo "$line" | sed 's/^  //; s/:.*//')
        echo "$entry"
      fi
    fi
  done < "$catalog_path"
}

# intent 필드가 빈 문자열인지 검사
check_empty_intents() {
  local catalog_path="$1"
  [[ ! -f "$catalog_path" ]] && return
  local current_file=""
  local in_files_section=0
  while IFS= read -r line; do
    if echo "$line" | grep -qE '^files:'; then
      in_files_section=1
      continue
    fi
    if [[ $in_files_section -eq 1 ]]; then
      if echo "$line" | grep -qE '^[a-zA-Z]'; then
        break
      fi
      # 파일명 추출
      if echo "$line" | grep -qE '^  [a-zA-Z0-9_][a-zA-Z0-9_./-]*:'; then
        current_file=$(echo "$line" | sed 's/^  //; s/:.*//')
      fi
      # intent 빈 문자열 검사
      if echo "$line" | grep -qE '^\s+intent:\s*""'; then
        ERRORS+=("[카탈로그] $catalog_path: '$current_file'의 intent가 빈 문자열")
      fi
      if echo "$line" | grep -qE "^\s+intent:\s*''"; then
        ERRORS+=("[카탈로그] $catalog_path: '$current_file'의 intent가 빈 문자열")
      fi
      # intent 필드 자체가 비어있는 경우 (intent: )
      if echo "$line" | grep -qE '^\s+intent:\s*$'; then
        ERRORS+=("[카탈로그] $catalog_path: '$current_file'의 intent가 비어 있음")
      fi
    fi
  done < "$catalog_path"
}

for dir in "${CORPUS_DIRS_TOUCHED[@]}"; do
  catalog_path="$dir/$CATALOG_FILE"

  # 디렉토리 내 실제 데이터 파일 목록 (DATA_CATALOG.yaml 제외)
  actual_files=()
  if [[ -d "$dir" ]]; then
    while IFS= read -r f; do
      [[ -z "$f" ]] && continue
      local_basename=$(basename "$f")
      [[ "$local_basename" == "$CATALOG_FILE" ]] && continue
      [[ "$local_basename" == ".gitkeep" ]] && continue
      actual_files+=("$local_basename")
    done < <(find "$dir" -maxdepth 1 -type f 2>/dev/null | sort)
  fi

  # 데이터 파일이 없으면 검증 불필요
  [[ ${#actual_files[@]} -eq 0 ]] && continue

  # 카탈로그 존재 검증
  if [[ ! -f "$catalog_path" ]]; then
    ERRORS+=("[카탈로그] $dir: 데이터 파일이 있지만 ${CATALOG_FILE}이 없음")
    continue
  fi

  # 카탈로그 엔트리 추출
  catalog_entries=()
  while IFS= read -r entry; do
    [[ -z "$entry" ]] && continue
    catalog_entries+=("$entry")
  done < <(extract_catalog_entries "$catalog_path")

  # 실제 파일 → 카탈로그 엔트리 존재 확인 (누락 검사)
  for actual in "${actual_files[@]}"; do
    found=0
    for entry in "${catalog_entries[@]+"${catalog_entries[@]}"}"; do
      [[ "$entry" == "$actual" ]] && found=1 && break
    done
    if [[ $found -eq 0 ]]; then
      ERRORS+=("[카탈로그] $dir: '$actual' 파일이 ${CATALOG_FILE}에 누락됨")
    fi
  done

  # 카탈로그 엔트리 → 실제 파일 존재 확인 (고스트 엔트리 검사)
  for entry in "${catalog_entries[@]+"${catalog_entries[@]}"}"; do
    found=0
    for actual in "${actual_files[@]}"; do
      [[ "$entry" == "$actual" ]] && found=1 && break
    done
    if [[ $found -eq 0 ]]; then
      ERRORS+=("[카탈로그] $dir: '$entry' 엔트리가 ${CATALOG_FILE}에 있지만 실제 파일 없음")
    fi
  done

  # 빈 intent 검사
  check_empty_intents "$catalog_path"
done

# --------------------------------------------------------------------------
# 결과 리포트
# --------------------------------------------------------------------------
if [[ ${#ERRORS[@]} -gt 0 ]]; then
  echo ""
  echo "[test-data] =========================================="
  echo "[test-data] 테스트 데이터 검증 실패 (${#ERRORS[@]}건)"
  echo "[test-data] =========================================="
  for err in "${ERRORS[@]}"; do
    echo "  - $err"
  done
  echo ""
  echo "[test-data] 수정 방법:"
  echo "  1. 파일명을 규칙에 맞게 변경 (docs/test-data-guidelines.md 참조)"
  echo "  2. DATA_CATALOG.yaml에 파일 엔트리 추가/제거"
  echo "  3. 예외 시 커밋 메시지에 'Test-Data-Doc: skip' 트레일러 추가"
  exit 1
fi

echo "[test-data] 테스트 데이터 검증 통과"
exit 0
