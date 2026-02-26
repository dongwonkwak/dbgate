#!/usr/bin/env bash
# =============================================================================
# dbgate 통합 스모크 테스트
#
# Phase 1 — MySQL 직접 접속: DDL/DML/SELECT 기본 동작 확인
# Phase 2 — 프록시 경유:     dbgate를 실제로 기동 후 쿼리 포워딩 검증
# Phase 3 — 정책 차단:       block_patterns(UNION SELECT, SLEEP) 차단 검증
#
# 환경변수 (integration.yml 에서 주입)
#   MYSQL_HOST, MYSQL_PORT, MYSQL_USER, MYSQL_PASSWORD, MYSQL_DATABASE
#   POLICY_PATH (사용 안 함 — 테스트 전용 정책을 자체 생성)
# =============================================================================
set -euo pipefail

MYSQL_HOST="${MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_USER="${MYSQL_USER:-testuser}"
MYSQL_PASSWORD="${MYSQL_PASSWORD:-testpass}"
MYSQL_DATABASE="${MYSQL_DATABASE:-testdb}"
PROXY_PORT="${PROXY_PORT:-13306}"
DBGATE_BIN="${DBGATE_BIN:-build/default/dbgate}"
TEST_POLICY="/tmp/dbgate-smoke-policy.yaml"

PASS=0
FAIL=0
DBGATE_PID=""

# ─── helpers ──────────────────────────────────────────────────────────────────

mysql_direct() {
    mysql -h "$MYSQL_HOST" -P "$MYSQL_PORT" \
          -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
          --connect-timeout=5 -e "$1" 2>/dev/null
}

mysql_proxy() {
    mysql -h 127.0.0.1 -P "$PROXY_PORT" \
          -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
          --connect-timeout=5 -e "$1" 2>/dev/null
}

pass() { echo "  ✅ $1"; PASS=$((PASS + 1)); }
fail() { echo "  ❌ $1"; FAIL=$((FAIL + 1)); }

run_ok() {
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc"; fi
}

run_block() {
    # 차단이 예상되는 케이스: 실패(접속 거부/에러)가 정상
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then
        fail "$desc (차단 예상이었으나 성공함)"
    else
        pass "$desc (예상대로 차단됨)"
    fi
}

cleanup() {
    if [ -n "$DBGATE_PID" ] && kill -0 "$DBGATE_PID" 2>/dev/null; then
        kill "$DBGATE_PID" 2>/dev/null || true
        wait "$DBGATE_PID" 2>/dev/null || true
    fi
    mysql_direct "DROP TABLE IF EXISTS smoke_direct" 2>/dev/null || true
    mysql_direct "DROP TABLE IF EXISTS smoke_proxy"  2>/dev/null || true
}
trap cleanup EXIT

# ─── Phase 1: MySQL 직접 접속 ─────────────────────────────────────────────────

echo ""
echo "=== Phase 1: MySQL 직접 접속 ==="

run_ok "SELECT 1 (연결 확인)"   mysql_direct "SELECT 1"
run_ok "SELECT VERSION()"        mysql_direct "SELECT VERSION()"
run_ok "CREATE TABLE"            mysql_direct \
    "CREATE TABLE IF NOT EXISTS smoke_direct (id INT AUTO_INCREMENT PRIMARY KEY, val VARCHAR(100))"
run_ok "INSERT"                  mysql_direct \
    "INSERT INTO smoke_direct (val) VALUES ('direct-ok')"
run_ok "SELECT (조회 확인)"      mysql_direct \
    "SELECT * FROM smoke_direct WHERE val='direct-ok'"
run_ok "DROP TABLE"              mysql_direct \
    "DROP TABLE IF EXISTS smoke_direct"

# ─── Phase 2 & 3: 프록시 경유 + 정책 차단 ────────────────────────────────────

echo ""
echo "=== Phase 2 & 3: dbgate 프록시 경유 + 정책 차단 ==="

if [ ! -x "$DBGATE_BIN" ]; then
    echo "  ⚠️  $DBGATE_BIN 없음 — 프록시 단계 건너뜀"
else
    # testuser 를 허용하는 테스트 전용 정책 (Phase 3에서 일부 패턴은 차단)
    cat > "$TEST_POLICY" <<'YAML'
global:
  log_level: warn
  log_format: json
  max_connections: 100
  connection_timeout: 30s
access_control:
  - user: "testuser"
    source_ip: "0.0.0.0/0"
    allowed_tables: ["*"]
    allowed_operations: ["SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP"]
    time_restriction: null
sql_rules:
  block_statements: []
  block_patterns:
    - "UNION\\s+SELECT"
    - "SLEEP\\s*\\("
procedure_control:
  mode: "whitelist"
  whitelist: []
  block_dynamic_sql: false
  block_create_alter: false
data_protection:
  max_result_rows: 10000
  block_schema_access: false
  sensitive_columns: []
alerts:
  on_block: false
  on_high_volume_query: false
  threshold_qps: 10000
YAML

    # 프록시 기동
    MYSQL_HOST="$MYSQL_HOST" \
    MYSQL_PORT="$MYSQL_PORT" \
    PROXY_LISTEN_PORT="$PROXY_PORT" \
    POLICY_PATH="$TEST_POLICY" \
    LOG_PATH="/tmp/dbgate-test.log" \
    LOG_LEVEL="warn" \
    UDS_SOCKET_PATH="/tmp/dbgate-test.sock" \
    HEALTH_CHECK_PORT="8081" \
        "$DBGATE_BIN" &
    DBGATE_PID=$!

    # 헬스체크 대기 (최대 10초)
    READY=0
    for i in $(seq 1 20); do
        if curl -sf http://127.0.0.1:8081/health >/dev/null 2>&1; then
            READY=1; break
        fi
        sleep 0.5
    done

    if [ $READY -eq 0 ]; then
        fail "dbgate 헬스체크 타임아웃 (10초)"
    else
        echo "  ℹ️  dbgate 기동 완료 (PID=$DBGATE_PID)"

        # Phase 2: 쿼리 포워딩
        run_ok  "프록시 경유 SELECT 1"  mysql_proxy "SELECT 1"
        run_ok  "프록시 경유 CREATE"    mysql_proxy \
            "CREATE TABLE IF NOT EXISTS smoke_proxy (id INT AUTO_INCREMENT PRIMARY KEY, val VARCHAR(100))"
        run_ok  "프록시 경유 INSERT"    mysql_proxy \
            "INSERT INTO smoke_proxy (val) VALUES ('proxy-ok')"
        run_ok  "프록시 경유 SELECT"    mysql_proxy \
            "SELECT * FROM smoke_proxy WHERE val='proxy-ok'"
        run_ok  "프록시 경유 DROP"      mysql_proxy \
            "DROP TABLE IF EXISTS smoke_proxy"

        # Phase 3: 정책 차단 검증
        run_block "UNION SELECT 차단"   mysql_proxy "SELECT 1 UNION SELECT 2"
        run_block "SLEEP() 차단"        mysql_proxy "SELECT SLEEP(0)"
    fi
fi

# ─── 결과 ─────────────────────────────────────────────────────────────────────

echo ""
echo "=== 결과: PASS=${PASS} / FAIL=${FAIL} ==="
if [ "$FAIL" -gt 0 ]; then
    echo "❌ 통합 테스트 실패"
    exit 1
fi
echo "✅ 통합 테스트 전체 통과"
