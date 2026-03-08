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
MYSQL_BIN="${MYSQL_BIN:-mysql}"

# mysql/mariadb 클라이언트 버전 차이를 흡수해 SSL 비활성 옵션을 결정한다.
MYSQL_SSL_ARGS=(--skip-ssl)
if "$MYSQL_BIN" --help 2>/dev/null | grep -q -- "--ssl-mode"; then
    MYSQL_SSL_ARGS=(--ssl-mode=DISABLED)
fi

PASS=0
FAIL=0
DBGATE_PID=""

# ─── helpers ──────────────────────────────────────────────────────────────────

mysql_direct() {
    "$MYSQL_BIN" "${MYSQL_SSL_ARGS[@]}" \
          -h "$MYSQL_HOST" -P "$MYSQL_PORT" \
          -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
          --connect-timeout=5 -e "$1" 2>/dev/null
}

mysql_proxy() {
    "$MYSQL_BIN" "${MYSQL_SSL_ARGS[@]}" \
          -h 127.0.0.1 -P "$PROXY_PORT" \
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
    mysql_direct "DROP TABLE IF EXISTS smoke_trunc"  2>/dev/null || true
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
    echo "  ❌ $DBGATE_BIN 없음 — 프록시 바이너리 누락으로 통합 테스트 실패"
    exit 1
else
    # MYSQL_USER 를 허용하는 테스트 전용 정책 (Phase 3에서 일부 패턴은 차단)
    cat > "$TEST_POLICY" <<'YAML'
global:
  log_level: warn
  log_format: json
  max_connections: 100
  connection_timeout: 30s
access_control:
  - user: "__MYSQL_USER__"
    source_ip: "0.0.0.0/0"
    allowed_tables: ["*"]
    allowed_operations: ["SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP"]
    time_restriction: null
sql_rules:
  block_statements: []
  block_patterns:
    - 'UNION\s+SELECT'
    - 'SLEEP\s*\('
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
    # heredoc 내 플레이스홀더를 실제 MYSQL_USER로 치환
    sed -i "s/__MYSQL_USER__/${MYSQL_USER}/g" "$TEST_POLICY"

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

# ─── Phase 4: block_statements 차단 ──────────────────────────────────────────

echo ""
echo "=== Phase 4: block_statements 차단 (DROP/TRUNCATE) ==="

if [ -n "$DBGATE_PID" ] && kill -0 "$DBGATE_PID" 2>/dev/null; then
    kill "$DBGATE_PID" 2>/dev/null || true
    wait "$DBGATE_PID" 2>/dev/null || true
    DBGATE_PID=""
fi

cat > "$TEST_POLICY" <<'YAML'
global:
  log_level: warn
  log_format: json
  max_connections: 100
  connection_timeout: 30s
access_control:
  - user: "__MYSQL_USER__"
    source_ip: "0.0.0.0/0"
    allowed_tables: ["*"]
    allowed_operations: ["SELECT", "INSERT", "UPDATE", "DELETE", "CREATE"]
    time_restriction: null
sql_rules:
  block_statements: ["DROP", "TRUNCATE"]
  # policy_loader fail-close 제약: block_patterns 는 최소 1개 필요
  # Phase 4 검증 대상은 DROP/TRUNCATE 이므로 기존 기본 패턴 1개만 유지
  block_patterns:
    - 'UNION\s+SELECT'
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
sed -i "s/__MYSQL_USER__/${MYSQL_USER}/g" "$TEST_POLICY"

# 프록시 재기동
MYSQL_HOST="$MYSQL_HOST" \
MYSQL_PORT="$MYSQL_PORT" \
PROXY_LISTEN_PORT="$PROXY_PORT" \
POLICY_PATH="$TEST_POLICY" \
LOG_PATH="/tmp/dbgate-test-p4.log" \
LOG_LEVEL="warn" \
UDS_SOCKET_PATH="/tmp/dbgate-test-p4.sock" \
HEALTH_CHECK_PORT="8082" \
    "$DBGATE_BIN" &
DBGATE_PID=$!

# 헬스체크 대기 (최대 10초)
READY=0
for i in $(seq 1 20); do
    if curl -sf http://127.0.0.1:8082/health >/dev/null 2>&1; then
        READY=1; break
    fi
    sleep 0.5
done

if [ $READY -eq 0 ]; then
    fail "Phase 4: dbgate 헬스체크 타임아웃"
else
    echo "  dbgate Phase 4 기동 완료 (PID=$DBGATE_PID)"

    # SELECT 는 통과
    run_ok    "Phase4: SELECT 허용"       mysql_proxy "SELECT 1"

    # TRUNCATE 차단 검증: 실존 테이블로 "table not found" 오류와 구별
    mysql_direct "CREATE TABLE IF NOT EXISTS smoke_trunc (id INT PRIMARY KEY)" 2>/dev/null || true

    # DROP/TRUNCATE 는 차단
    run_block "Phase4: DROP TABLE 차단"   mysql_proxy "DROP TABLE IF EXISTS nonexistent_smoke"
    run_block "Phase4: TRUNCATE 차단"     mysql_proxy "TRUNCATE TABLE smoke_trunc"
fi

# ─── Phase 5: 시간대 차단 ─────────────────────────────────────────────────────

echo ""
echo "=== Phase 5: 시간대 차단 (time_restriction) ==="

# 현재 UTC 시/분 확인
CURRENT_UTC_HOUR=$(date -u +%H)
CURRENT_UTC_MIN=$(date -u +%M)

# 현재 시각을 포함하지 않는 허용 범위 = 현재+2시간 ~ 현재+3시간 (UTC)
# → 현재 시각은 항상 차단됨
ALLOW_HOUR=$(( (10#$CURRENT_UTC_HOUR + 2) % 24 ))
ALLOW_END=$(( (10#$CURRENT_UTC_HOUR + 3) % 24 ))
ALLOW_START_STR=$(printf "%02d:00" "$ALLOW_HOUR")
ALLOW_END_STR=$(printf "%02d:00" "$ALLOW_END")

# dbgate 재기동
if [ -n "$DBGATE_PID" ] && kill -0 "$DBGATE_PID" 2>/dev/null; then
    kill "$DBGATE_PID" 2>/dev/null || true
    wait "$DBGATE_PID" 2>/dev/null || true
    DBGATE_PID=""
fi

cat > "$TEST_POLICY" <<YAML
global:
  log_level: warn
  log_format: json
  max_connections: 100
  connection_timeout: 30s
access_control:
  - user: "${MYSQL_USER}"
    source_ip: "0.0.0.0/0"
    allowed_tables: ["*"]
    allowed_operations: ["SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP"]
    time_restriction:
      allow: "${ALLOW_START_STR}-${ALLOW_END_STR}"
      timezone: "UTC"
sql_rules:
  block_statements: []
  # policy_loader fail-close 제약: block_patterns 는 최소 1개 필요
  # Phase 5 검증 대상은 time_restriction 이므로 기본 패턴 1개만 유지
  block_patterns:
    - 'UNION\s+SELECT'
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

MYSQL_HOST="$MYSQL_HOST" \
MYSQL_PORT="$MYSQL_PORT" \
PROXY_LISTEN_PORT="$PROXY_PORT" \
POLICY_PATH="$TEST_POLICY" \
LOG_PATH="/tmp/dbgate-test-p5.log" \
LOG_LEVEL="warn" \
UDS_SOCKET_PATH="/tmp/dbgate-test-p5.sock" \
HEALTH_CHECK_PORT="8083" \
    "$DBGATE_BIN" &
DBGATE_PID=$!

READY=0
for i in $(seq 1 20); do
    if curl -sf http://127.0.0.1:8083/health >/dev/null 2>&1; then
        READY=1; break
    fi
    sleep 0.5
done

if [ $READY -eq 0 ]; then
    fail "Phase 5: dbgate 헬스체크 타임아웃"
else
    echo "  dbgate Phase 5 기동 완료 (허용 범위: ${ALLOW_START_STR}~${ALLOW_END_STR} UTC, 현재 UTC: ${CURRENT_UTC_HOUR}:${CURRENT_UTC_MIN})"
    run_block "Phase5: 시간대 외 SELECT 차단" mysql_proxy "SELECT 1"
fi

# ─── 결과 ─────────────────────────────────────────────────────────────────────

echo ""
echo "=== 결과: PASS=${PASS} / FAIL=${FAIL} ==="
if [ "$FAIL" -gt 0 ]; then
    echo "통합 테스트 실패"
    exit 1
fi
echo "통합 테스트 전체 통과"
