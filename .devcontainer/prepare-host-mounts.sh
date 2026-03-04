#!/usr/bin/env bash
set -euo pipefail

home_dir="${HOME}"

ensure_dir() {
  local path="$1"
  if [ ! -d "$path" ]; then
    mkdir -p "$path"
  fi
}

ensure_json_file() {
  local path="$1"
  if [ ! -e "$path" ]; then
    printf '{}\n' >"$path"
  fi
}

ensure_file() {
  local path="$1"
  if [ ! -e "$path" ]; then
    : >"$path"
  fi
}

ensure_dir "${home_dir}/.codex"
ensure_dir "${home_dir}/.claude"
ensure_dir "${home_dir}/.claude/plugins"
ensure_dir "${home_dir}/.config/gh"

ensure_json_file "${home_dir}/.codex/auth.json"
ensure_json_file "${home_dir}/.codex/.credentials.json"
ensure_file "${home_dir}/.codex/config.toml"
ensure_json_file "${home_dir}/.claude/.claude.json"
ensure_json_file "${home_dir}/.claude/.credentials.json"
ensure_json_file "${home_dir}/.claude/settings.json"
