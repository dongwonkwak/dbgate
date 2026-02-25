#!/usr/bin/env bash
# git hooks 설치 스크립트.
# scripts/hooks/ 의 훅을 .git/hooks/ 에 심볼릭 링크로 설치한다.
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
HOOKS_SRC="$REPO_ROOT/scripts/hooks"
HOOKS_DEST="$REPO_ROOT/.git/hooks"

if [[ ! -d "$HOOKS_DEST" ]]; then
  echo "오류: .git/hooks 디렉토리를 찾을 수 없습니다. git 저장소인지 확인하세요." >&2
  exit 1
fi

installed=0
for hook_file in "$HOOKS_SRC"/*; do
  [[ -f "$hook_file" ]] || continue
  name="$(basename "$hook_file")"
  dest="$HOOKS_DEST/$name"

  # 기존 훅이 심볼릭 링크가 아닌 파일이면 백업
  if [[ -f "$dest" && ! -L "$dest" ]]; then
    mv "$dest" "${dest}.bak"
    echo "  기존 $name 훅을 ${name}.bak 으로 백업했습니다."
  fi

  ln -sf "../../scripts/hooks/$name" "$dest"
  chmod +x "$hook_file"
  echo "  설치됨: $name -> scripts/hooks/$name"
  ((installed++)) || true
done

if [[ $installed -eq 0 ]]; then
  echo "설치할 훅이 없습니다. scripts/hooks/ 를 확인하세요."
else
  echo ""
  echo "총 ${installed}개 훅 설치 완료."
fi
