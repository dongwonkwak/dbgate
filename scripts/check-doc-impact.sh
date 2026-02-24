#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/check-doc-impact.sh --base <git-ref> --head <git-ref>

Exit codes:
  0: pass
  1: doc impact missing
  2: usage/input/parsing error
EOF
}

BASE_REF=""
HEAD_REF=""
OWNERSHIP_MAP=".claude/ownership-map.yaml"

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

if [[ ! -f "$OWNERSHIP_MAP" ]]; then
  echo "Ownership map not found: $OWNERSHIP_MAP" >&2
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

RANGE_SPEC="${BASE_REF}..${HEAD_REF}"
CHANGED_FILES="$(git diff --name-only "$RANGE_SPEC")"
COMMIT_MESSAGES="$(git log --format=%B "$RANGE_SPEC" || true)"

if [[ -z "$CHANGED_FILES" ]]; then
  echo "No changed files in range: $RANGE_SPEC"
  exit 0
fi

if ! command -v ruby >/dev/null 2>&1; then
  echo "ruby is required to parse $OWNERSHIP_MAP" >&2
  exit 2
fi

export DOC_IMPACT_RANGE_SPEC="$RANGE_SPEC"
export DOC_IMPACT_CHANGED_FILES="$CHANGED_FILES"
export DOC_IMPACT_COMMIT_MESSAGES="$COMMIT_MESSAGES"
export DOC_IMPACT_OWNERSHIP_MAP="$OWNERSHIP_MAP"

ruby <<'RUBY'
require "yaml"

def env!(key)
  v = ENV[key]
  raise "missing env #{key}" if v.nil?
  v
end

def is_doc_path?(path)
  path == "README.md" || path.start_with?("docs/")
end

def trailer_exception?(commit_messages)
  has_flag = commit_messages.match?(/^Docs-Impact:\s*none\s*$/i)
  has_reason = commit_messages.match?(/^Docs-Impact-Reason:\s*.+$/i)
  has_flag && has_reason
end

def rule_matches_path?(rule, path)
  Array(rule["match"]).any? do |glob|
    File.fnmatch?(glob, path, File::FNM_PATHNAME)
  end
end

def rule_label(rule)
  id = rule["id"] || "(unnamed)"
  patterns = Array(rule["match"])
  return id if patterns.empty?
  "#{id} (#{patterns.join(', ')})"
end

begin
  range_spec = env!("DOC_IMPACT_RANGE_SPEC")
  changed_files = env!("DOC_IMPACT_CHANGED_FILES").split("\n").reject(&:empty?).uniq
  commit_messages = env!("DOC_IMPACT_COMMIT_MESSAGES")
  ownership_map_path = env!("DOC_IMPACT_OWNERSHIP_MAP")

  map = YAML.load_file(ownership_map_path)
  path_rules = Array(map["path_rules"])

  changed_docs = changed_files.select { |p| is_doc_path?(p) }
  changed_non_docs = changed_files.reject { |p| is_doc_path?(p) }

  if changed_non_docs.empty?
    puts "PASS: docs-only change (no non-doc files changed)."
    unless changed_docs.empty?
      puts "Changed docs:"
      changed_docs.each { |p| puts "  - #{p}" }
    end
    exit 0
  end

  triggered_rules = path_rules.select do |rule|
    changed_non_docs.any? { |path| rule_matches_path?(rule, path) }
  end

  required_rules = triggered_rules.select do |rule|
    rule.dig("doc_impact", "required") == true
  end

  if required_rules.empty?
    puts "PASS: no doc-impact-required path groups matched."
    puts "Changed non-doc files:"
    changed_non_docs.each { |p| puts "  - #{p}" }
    exit 0
  end

  missing_rules = required_rules.select do |rule|
    candidates = Array(rule.dig("doc_impact", "candidates"))
    candidates.none? { |c| changed_docs.include?(c) }
  end

  if !missing_rules.empty? && trailer_exception?(commit_messages)
    puts "PASS: doc-impact exception trailer found in commit messages."
    puts
    puts "Missing doc groups (ignored by trailer):"
    missing_rules.each { |rule| puts "  - #{rule_label(rule)}" }
    exit 0
  end

  puts "Doc impact check summary"
  puts "Range: #{range_spec}"
  puts
  puts "Changed non-doc files:"
  changed_non_docs.each { |p| puts "  - #{p}" }
  puts
  puts "Changed docs:"
  if changed_docs.empty?
    puts "  (none)"
  else
    changed_docs.each { |p| puts "  - #{p}" }
  end
  puts
  puts "Triggered groups:"
  triggered_rules.each do |rule|
    puts "  - #{rule_label(rule)}"
    puts "    candidates:"
    Array(rule.dig("doc_impact", "candidates")).each do |c|
      puts "      - #{c}"
    end
  end

  if missing_rules.empty?
    puts
    puts "PASS: each required triggered group has at least one matching doc update."
    exit 0
  end

  puts
  puts "FAIL: missing doc updates for required groups:"
  missing_rules.each { |rule| puts "  - #{rule_label(rule)}" }
  puts
  puts "If this is an exception, add commit trailers:"
  puts "  Docs-Impact: none"
  puts "  Docs-Impact-Reason: <specific reason>"
  exit 1
rescue Psych::SyntaxError => e
  warn "Failed to parse ownership map YAML: #{e.message}"
  exit 2
rescue => e
  warn "Doc impact check internal error: #{e.class}: #{e.message}"
  exit 2
end
RUBY
