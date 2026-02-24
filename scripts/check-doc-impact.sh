#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/check-doc-impact.sh --base <git-ref> --head <git-ref>

Exit codes:
  0: pass
  1: doc impact missing
  2: usage/input error
EOF
}

BASE_REF=""
HEAD_REF=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      BASE_REF="$2"
      shift 2
      ;;
    --head)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      HEAD_REF="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ -z "$BASE_REF" || -z "$HEAD_REF" ]]; then
  echo "Both --base and --head are required." >&2
  usage
  exit 2
fi

if ! git rev-parse --verify "$BASE_REF" >/dev/null 2>&1; then
  echo "Invalid base ref: $BASE_REF" >&2
  exit 2
fi

if ! git rev-parse --verify "$HEAD_REF" >/dev/null 2>&1; then
  echo "Invalid head ref: $HEAD_REF" >&2
  exit 2
fi

append_unique_line() {
  # $1: current lines (possibly empty), $2: new value
  local current="$1"
  local value="$2"
  if [[ -z "$value" ]]; then
    printf '%s' "$current"
    return
  fi
  if [[ -z "$current" ]]; then
    printf '%s' "$value"
    return
  fi
  if printf '%s\n' "$current" | grep -Fx -- "$value" >/dev/null 2>&1; then
    printf '%s' "$current"
  else
    printf '%s\n%s' "$current" "$value"
  fi
}

is_doc_path() {
  case "$1" in
    docs/*|README.md) return 0 ;;
    *) return 1 ;;
  esac
}

matches_group() {
  local group="$1"
  local path="$2"
  case "$group" in
    network)
      case "$path" in src/protocol/*|src/proxy/*|src/health/*) return 0 ;; esac
      ;;
    security)
      case "$path" in src/parser/*|src/policy/*|config/policy.yaml) return 0 ;; esac
      ;;
    infra_obs)
      case "$path" in src/logger/*|src/stats/*) return 0 ;; esac
      ;;
    go_tools)
      case "$path" in tools/*) return 0 ;; esac
      ;;
    deploy_ci)
      case "$path" in deploy/*|.github/workflows/*|docker-compose.yaml|.devcontainer/*) return 0 ;; esac
      ;;
    qa)
      case "$path" in tests/*|benchmarks/*) return 0 ;; esac
      ;;
  esac
  return 1
}

group_required() {
  case "$1" in
    qa) echo "false" ;;
    *) echo "true" ;;
  esac
}

group_candidates() {
  case "$1" in
    network)
      cat <<'EOF'
docs/architecture.md
docs/data-flow.md
docs/sequences.md
docs/interface-reference.md
EOF
      ;;
    security)
      cat <<'EOF'
docs/policy-engine.md
docs/data-flow.md
docs/interface-reference.md
docs/threat-model.md
EOF
      ;;
    infra_obs)
      cat <<'EOF'
docs/observability.md
docs/failure-modes.md
docs/uds-protocol.md
docs/interface-reference.md
EOF
      ;;
    go_tools)
      cat <<'EOF'
README.md
docs/runbook.md
docs/uds-protocol.md
docs/observability.md
docs/interface-reference.md
EOF
      ;;
    deploy_ci)
      cat <<'EOF'
docs/runbook.md
docs/failure-modes.md
README.md
EOF
      ;;
    qa)
      cat <<'EOF'
docs/testing-strategy.md
README.md
EOF
      ;;
    *)
      ;;
  esac
}

group_label() {
  case "$1" in
    network) echo "network (src/protocol|proxy|health)" ;;
    security) echo "security (src/parser|policy|config/policy.yaml)" ;;
    infra_obs) echo "infra-observability (src/logger|stats)" ;;
    go_tools) echo "go-tools (tools/)" ;;
    deploy_ci) echo "deploy-ci (deploy|workflows|docker-compose|devcontainer)" ;;
    qa) echo "qa (tests|benchmarks)" ;;
    *) echo "$1" ;;
  esac
}

group_has_doc_match() {
  local group="$1"
  local changed_docs="$2"
  local c
  while IFS= read -r c; do
    [[ -n "$c" ]] || continue
    if printf '%s\n' "$changed_docs" | grep -Fx -- "$c" >/dev/null 2>&1; then
      return 0
    fi
  done < <(group_candidates "$group")
  return 1
}

range_spec="${BASE_REF}..${HEAD_REF}"

changed_files="$(git diff --name-only "$range_spec")"

if [[ -z "$changed_files" ]]; then
  echo "No changed files in range: $range_spec"
  exit 0
fi

all_changed=""
changed_docs=""
changed_non_docs=""

while IFS= read -r path; do
  [[ -n "$path" ]] || continue
  all_changed="$(append_unique_line "$all_changed" "$path")"
  if is_doc_path "$path"; then
    changed_docs="$(append_unique_line "$changed_docs" "$path")"
  else
    changed_non_docs="$(append_unique_line "$changed_non_docs" "$path")"
  fi
done < <(printf '%s\n' "$changed_files")

if [[ -z "$changed_non_docs" ]]; then
  echo "PASS: docs-only change (no non-doc files changed)."
  if [[ -n "$changed_docs" ]]; then
    echo "Changed docs:"
    printf '  - %s\n' $changed_docs
  fi
  exit 0
fi

groups=("network" "security" "infra_obs" "go_tools" "deploy_ci" "qa")
triggered_groups=""
required_triggered_groups=""

for group in "${groups[@]}"; do
  while IFS= read -r path; do
    [[ -n "$path" ]] || continue
    if matches_group "$group" "$path"; then
      triggered_groups="$(append_unique_line "$triggered_groups" "$group")"
      if [[ "$(group_required "$group")" == "true" ]]; then
        required_triggered_groups="$(append_unique_line "$required_triggered_groups" "$group")"
      fi
      break
    fi
  done < <(printf '%s\n' "$changed_non_docs")
done

if [[ -z "$required_triggered_groups" ]]; then
  echo "PASS: no doc-impact-required path groups matched."
  echo "Changed non-doc files:"
  printf '  - %s\n' $changed_non_docs
  exit 0
fi

missing_groups=""
for group in $(printf '%s\n' "$required_triggered_groups"); do
  if ! group_has_doc_match "$group" "$changed_docs"; then
    missing_groups="$(append_unique_line "$missing_groups" "$group")"
  fi
done

if [[ -n "$missing_groups" ]]; then
  commit_messages="$(git log --format=%B "$range_spec" || true)"
  if printf '%s\n' "$commit_messages" | grep -Eiq '^Docs-Impact:[[:space:]]*none[[:space:]]*$' \
    && printf '%s\n' "$commit_messages" | grep -Eiq '^Docs-Impact-Reason:[[:space:]]*.+$'; then
    echo "PASS: doc-impact exception trailer found in commit messages."
    echo
    echo "Missing doc groups (ignored by trailer):"
    while IFS= read -r group; do
      [[ -n "$group" ]] || continue
      echo "  - $(group_label "$group")"
    done < <(printf '%s\n' "$missing_groups")
    exit 0
  fi
fi

echo "Doc impact check summary"
echo "Range: $range_spec"
echo
echo "Changed non-doc files:"
while IFS= read -r path; do
  [[ -n "$path" ]] || continue
  echo "  - $path"
done < <(printf '%s\n' "$changed_non_docs")

echo
echo "Changed docs:"
if [[ -n "$changed_docs" ]]; then
  while IFS= read -r path; do
    [[ -n "$path" ]] || continue
    echo "  - $path"
  done < <(printf '%s\n' "$changed_docs")
else
  echo "  (none)"
fi

echo
echo "Triggered groups:"
while IFS= read -r group; do
  [[ -n "$group" ]] || continue
  echo "  - $(group_label "$group")"
  echo "    candidates:"
  while IFS= read -r c; do
    [[ -n "$c" ]] || continue
    echo "      - $c"
  done < <(group_candidates "$group")
done < <(printf '%s\n' "$triggered_groups")

if [[ -z "$missing_groups" ]]; then
  echo
  echo "PASS: each required triggered group has at least one matching doc update."
  exit 0
fi

echo
echo "FAIL: missing doc updates for required groups:"
while IFS= read -r group; do
  [[ -n "$group" ]] || continue
  echo "  - $(group_label "$group")"
done < <(printf '%s\n' "$missing_groups")
echo
echo "If this is an exception, add commit trailers:"
echo "  Docs-Impact: none"
echo "  Docs-Impact-Reason: <specific reason>"
exit 1
