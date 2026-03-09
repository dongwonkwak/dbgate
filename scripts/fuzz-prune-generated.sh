#!/usr/bin/env bash
# fuzz-prune-generated.sh
# - 기본: tests/fuzz/generated 에서 N일 초과 파일만 삭제 (기본 3일)
# - --all: generated 파일 전체 삭제(.gitkeep 제외)
# - --seeds-hash: tests/fuzz/seeds 아래 해시형(untracked) 파일 삭제
set -euo pipefail

GENERATED_ROOT="tests/fuzz/generated"
SEED_ROOT="tests/fuzz/seeds"
MODE="aged"
DAYS=3
PRUNE_SEEDS_HASH=0

usage() {
  cat <<'USAGE'
Usage:
  bash scripts/fuzz-prune-generated.sh [--all] [--days N] [--seeds-hash]

Options:
  --all         generated 파일을 모두 삭제(.gitkeep 제외)
  --days N      N일 초과 파일만 삭제 (기본: 3)
  --seeds-hash  seeds 아래 해시형(untracked) 파일도 함께 삭제
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --all)
      MODE="all"
      shift
      ;;
    --days)
      DAYS="$2"
      shift 2
      ;;
    --seeds-hash)
      PRUNE_SEEDS_HASH=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[fuzz-prune] 알 수 없는 옵션: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if ! [[ "$DAYS" =~ ^[0-9]+$ ]]; then
  echo "[fuzz-prune] --days 값은 0 이상의 정수여야 합니다: $DAYS" >&2
  exit 2
fi

removed_generated=0
if [[ -d "$GENERATED_ROOT" ]]; then
  if [[ "$MODE" == "all" ]]; then
    while IFS= read -r f; do
      [[ -z "$f" ]] && continue
      rm -f -- "$f"
      removed_generated=$((removed_generated + 1))
    done < <(find "$GENERATED_ROOT" -type f ! -name '.gitkeep')
  else
    while IFS= read -r f; do
      [[ -z "$f" ]] && continue
      rm -f -- "$f"
      removed_generated=$((removed_generated + 1))
    done < <(find "$GENERATED_ROOT" -type f ! -name '.gitkeep' -mtime +"$DAYS")
  fi
fi

removed_seeds_hash=0
if [[ "$PRUNE_SEEDS_HASH" -eq 1 && -d "$SEED_ROOT" ]]; then
  while IFS= read -r p; do
    [[ -z "$p" ]] && continue
    rm -f -- "$p"
    removed_seeds_hash=$((removed_seeds_hash + 1))
  done < <(
    git ls-files --others --exclude-standard "$SEED_ROOT" \
      | awk -F/ '{n=$NF; if (n ~ /^[0-9a-f]{32,}$/) print}'
  )
fi

echo "[fuzz-prune] generated 정리 수: $removed_generated"
if [[ "$PRUNE_SEEDS_HASH" -eq 1 ]]; then
  echo "[fuzz-prune] seeds 해시 정리 수: $removed_seeds_hash"
fi
