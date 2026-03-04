#!/usr/bin/env bash
# =============================================================================
# dbgate sysbench 벤치마크: 직접연결 vs 프록시
#
# 사용법:
#   ./benchmarks/run_benchmark.sh [옵션]
#
# 옵션:
#   --threads N       sysbench 스레드 수 (기본: 4)
#   --time N          실행 시간 초 (기본: 30)
#   --table-size N    테이블 행 수 (기본: 10000)
#   --tables N        테이블 수 (기본: 4)
#   --skip-build      dbgate 빌드 건너뛰기
#   --workloads LIST  콤마 구분 워크로드 (기본: oltp_read_only,oltp_read_write)
# =============================================================================
set -euo pipefail

# ─── 프로젝트 루트 ────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# ─── 기본값 ───────────────────────────────────────────────────────────────────
THREADS=4
TIME=30
TABLE_SIZE=10000
TABLES=4
SKIP_BUILD=false
WORKLOADS="oltp_read_only,oltp_read_write"

MYSQL_HOST="${MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_USER="${MYSQL_USER:-dbgate}"
MYSQL_PASSWORD="${MYSQL_PASSWORD:-dbgate_pass}"
MYSQL_DATABASE="${MYSQL_DATABASE:-dbgate_bench}"
PROXY_PORT="${PROXY_PORT:-13306}"
HEALTH_CHECK_PORT="${HEALTH_CHECK_PORT:-8084}"
DBGATE_BIN="${DBGATE_BIN:-build/default/dbgate}"
POLICY_PATH="${POLICY_PATH:-benchmarks/policy-benchmark.yaml}"
RESULTS_DIR="benchmarks/results"

DBGATE_PID=""
DOCKER_STARTED=false

# ─── CLI 파싱 ─────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --threads)    THREADS="$2";     shift 2 ;;
        --time)       TIME="$2";        shift 2 ;;
        --table-size) TABLE_SIZE="$2";  shift 2 ;;
        --tables)     TABLES="$2";      shift 2 ;;
        --skip-build) SKIP_BUILD=true;  shift   ;;
        --workloads)  WORKLOADS="$2";   shift 2 ;;
        *)
            echo "알 수 없는 옵션: $1"
            exit 1
            ;;
    esac
done

IFS=',' read -ra WORKLOAD_LIST <<< "$WORKLOADS"

# ─── 전제조건 확인 ────────────────────────────────────────────────────────────
check_prereqs() {
    local missing=()
    for cmd in sysbench jq mysql; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "ERROR: 필수 도구가 설치되지 않음: ${missing[*]}"
        exit 1
    fi
}

# ─── 정리 함수 ────────────────────────────────────────────────────────────────
cleanup() {
    echo ""
    echo "=== 정리 ==="

    # dbgate 프로세스 종료
    if [[ -n "$DBGATE_PID" ]] && kill -0 "$DBGATE_PID" 2>/dev/null; then
        echo "  dbgate 종료 (PID=$DBGATE_PID)"
        kill "$DBGATE_PID" 2>/dev/null || true
        wait "$DBGATE_PID" 2>/dev/null || true
    fi

    # sysbench cleanup
    echo "  sysbench cleanup"
    sysbench oltp_read_only \
        --mysql-host="$MYSQL_HOST" --mysql-port="$MYSQL_PORT" \
        --mysql-user="$MYSQL_USER" --mysql-password="$MYSQL_PASSWORD" \
        --mysql-db="$MYSQL_DATABASE" \
        --tables="$TABLES" --table-size="$TABLE_SIZE" \
        cleanup 2>/dev/null || true

    # Docker 정리 (우리가 시작한 경우만)
    if [[ "$DOCKER_STARTED" == true ]]; then
        echo "  Docker 컨테이너 종료"
        docker compose -f benchmarks/docker-compose.benchmark.yml down 2>/dev/null || true
    fi

    echo "  정리 완료"
}
trap cleanup EXIT

# ─── MySQL 대기 ───────────────────────────────────────────────────────────────
wait_for_mysql() {
    echo "  MySQL 헬스체크 대기..."
    for i in $(seq 1 60); do
        if mysql -h "$MYSQL_HOST" -P "$MYSQL_PORT" \
                 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" \
                 --connect-timeout=3 -e "SELECT 1" &>/dev/null; then
            echo "  MySQL 준비 완료"
            return 0
        fi
        sleep 1
    done
    echo "ERROR: MySQL 연결 실패 (60초 타임아웃)"
    return 1
}

# ─── sysbench 결과 파싱 ───────────────────────────────────────────────────────
parse_sysbench_output() {
    local output="$1"
    local tps qps lat_avg lat_p95 lat_p99

    tps=$(echo "$output"    | grep "transactions:"    | awk '{print $3}' | tr -d '(' || true)
    qps=$(echo "$output"    | grep "queries:"         | awk '{print $3}' | tr -d '(' || true)
    lat_avg=$(echo "$output" | grep "avg:"             | awk '{print $2}' || true)
    lat_p95=$(echo "$output" | grep "95th percentile:" | awk '{print $3}' || true)
    lat_p99=$(echo "$output" | grep "99th percentile:" | awk '{print $3}' || true)

    echo "${tps:-0} ${qps:-0} ${lat_avg:-0} ${lat_p95:-0} ${lat_p99:-0}"
}

# ─── sysbench 실행 ────────────────────────────────────────────────────────────
run_sysbench() {
    local workload="$1"
    local host="$2"
    local port="$3"
    local label="$4"

    echo "  [$label] $workload (host=$host:$port, threads=$THREADS, time=${TIME}s)" >&2

    local output
    output=$(sysbench "$workload" \
        --mysql-host="$host" --mysql-port="$port" \
        --mysql-user="$MYSQL_USER" --mysql-password="$MYSQL_PASSWORD" \
        --mysql-db="$MYSQL_DATABASE" \
        --tables="$TABLES" --table-size="$TABLE_SIZE" \
        --threads="$THREADS" --time="$TIME" \
        --report-interval=0 \
        run 2>&1)

    parse_sysbench_output "$output"
}

# ─── 오버헤드 계산 ────────────────────────────────────────────────────────────
calc_overhead() {
    local direct="$1"
    local proxy="$2"
    if (( $(echo "$direct == 0" | bc -l) )); then
        echo "0"
    else
        echo "scale=2; ($proxy - $direct) / $direct * 100" | bc -l
    fi
}

# ─── 메인 ─────────────────────────────────────────────────────────────────────
main() {
    echo "=========================================="
    echo " dbgate sysbench 벤치마크"
    echo "=========================================="
    echo "  threads=$THREADS, time=${TIME}s, table-size=$TABLE_SIZE, tables=$TABLES"
    echo "  workloads: ${WORKLOAD_LIST[*]}"
    echo ""

    # 1. 전제조건 확인
    echo "=== 전제조건 확인 ==="
    check_prereqs
    echo "  sysbench, jq, mysql 확인 완료"

    # 2. dbgate 빌드
    if [[ "$SKIP_BUILD" == false ]]; then
        echo ""
        echo "=== dbgate 빌드 ==="
        cmake --preset default 2>&1 | tail -1
        cmake --build build/default 2>&1 | tail -1
        echo "  빌드 완료"
    fi

    if [[ ! -x "$DBGATE_BIN" ]]; then
        echo "ERROR: $DBGATE_BIN 바이너리 없음"
        exit 1
    fi

    # 3. MySQL 준비
    echo ""
    echo "=== MySQL 준비 ==="
    if mysql -h "$MYSQL_HOST" -P "$MYSQL_PORT" \
             -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" \
             --connect-timeout=3 -e "SELECT 1" &>/dev/null; then
        echo "  기존 MySQL 인스턴스 사용 ($MYSQL_HOST:$MYSQL_PORT)"
    else
        echo "  Docker로 MySQL 기동..."
        docker compose -f benchmarks/docker-compose.benchmark.yml up -d
        DOCKER_STARTED=true
        wait_for_mysql
    fi

    # 데이터베이스 생성 (존재하지 않을 경우)
    mysql -h "$MYSQL_HOST" -P "$MYSQL_PORT" \
          -u root -prootpass \
          --connect-timeout=5 \
          -e "CREATE DATABASE IF NOT EXISTS $MYSQL_DATABASE" 2>/dev/null || true

    # 4. sysbench prepare
    echo ""
    echo "=== sysbench prepare ==="
    sysbench oltp_read_only \
        --mysql-host="$MYSQL_HOST" --mysql-port="$MYSQL_PORT" \
        --mysql-user="$MYSQL_USER" --mysql-password="$MYSQL_PASSWORD" \
        --mysql-db="$MYSQL_DATABASE" \
        --tables="$TABLES" --table-size="$TABLE_SIZE" \
        prepare 2>&1 | tail -3
    echo "  prepare 완료"

    # 결과 저장용
    mkdir -p "$RESULTS_DIR"
    local timestamp
    timestamp=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

    # JSON 초기화
    local json_results="{}"

    for workload in "${WORKLOAD_LIST[@]}"; do
        echo ""
        echo "=== 워크로드: $workload ==="

        # 5. 직접 연결 벤치마크
        read -r d_tps d_qps d_avg d_p95 d_p99 <<< \
            "$(run_sysbench "$workload" "$MYSQL_HOST" "$MYSQL_PORT" "직접연결")"
        echo "    TPS=$d_tps  QPS=$d_qps  avg=${d_avg}ms  P95=${d_p95}ms  P99=${d_p99}ms"

        # 6. 프록시 기동
        if [[ -n "$DBGATE_PID" ]] && kill -0 "$DBGATE_PID" 2>/dev/null; then
            kill "$DBGATE_PID" 2>/dev/null || true
            wait "$DBGATE_PID" 2>/dev/null || true
            DBGATE_PID=""
        fi

        MYSQL_HOST="$MYSQL_HOST" \
        MYSQL_PORT="$MYSQL_PORT" \
        PROXY_LISTEN_PORT="$PROXY_PORT" \
        POLICY_PATH="$POLICY_PATH" \
        LOG_PATH="/tmp/dbgate-bench.log" \
        LOG_LEVEL="warn" \
        UDS_SOCKET_PATH="/tmp/dbgate-bench.sock" \
        HEALTH_CHECK_PORT="$HEALTH_CHECK_PORT" \
            "$DBGATE_BIN" &
        DBGATE_PID=$!

        # 헬스체크 대기
        local ready=0
        for i in $(seq 1 20); do
            if curl -sf "http://127.0.0.1:${HEALTH_CHECK_PORT}/health" >/dev/null 2>&1; then
                ready=1; break
            fi
            sleep 0.5
        done

        if [[ $ready -eq 0 ]]; then
            echo "  ERROR: dbgate 헬스체크 타임아웃"
            exit 1
        fi

        # 7. 프록시 경유 벤치마크
        read -r p_tps p_qps p_avg p_p95 p_p99 <<< \
            "$(run_sysbench "$workload" "127.0.0.1" "$PROXY_PORT" "프록시경유")"
        echo "    TPS=$p_tps  QPS=$p_qps  avg=${p_avg}ms  P95=${p_p95}ms  P99=${p_p99}ms"

        # 프록시 종료
        kill "$DBGATE_PID" 2>/dev/null || true
        wait "$DBGATE_PID" 2>/dev/null || true
        DBGATE_PID=""

        # 오버헤드 계산 (TPS는 낮을수록 나쁨 → 역방향)
        local oh_tps oh_p95 oh_p99
        oh_tps=$(calc_overhead "$d_tps" "$p_tps")
        oh_p95=$(calc_overhead "$d_p95" "$p_p95")
        oh_p99=$(calc_overhead "$d_p99" "$p_p99")

        # JSON 결과 누적
        json_results=$(echo "$json_results" | jq \
            --arg wl "$workload" \
            --argjson d_tps "$d_tps" --argjson d_qps "$d_qps" \
            --argjson d_avg "$d_avg" --argjson d_p95 "$d_p95" --argjson d_p99 "$d_p99" \
            --argjson p_tps "$p_tps" --argjson p_qps "$p_qps" \
            --argjson p_avg "$p_avg" --argjson p_p95 "$p_p95" --argjson p_p99 "$p_p99" \
            --argjson oh_tps "$oh_tps" --argjson oh_p95 "$oh_p95" --argjson oh_p99 "$oh_p99" \
            '.[$wl] = {
                "direct": {
                    "tps": $d_tps, "qps": $d_qps,
                    "latency_avg_ms": $d_avg, "latency_p95_ms": $d_p95, "latency_p99_ms": $d_p99
                },
                "proxy": {
                    "tps": $p_tps, "qps": $p_qps,
                    "latency_avg_ms": $p_avg, "latency_p95_ms": $p_p95, "latency_p99_ms": $p_p99
                },
                "overhead": {
                    "tps_percent": $oh_tps,
                    "latency_p95_percent": $oh_p95,
                    "latency_p99_percent": $oh_p99
                }
            }')
    done

    # 9. JSON 출력 생성
    local final_json
    final_json=$(jq -n \
        --arg ts "$timestamp" \
        --argjson threads "$THREADS" \
        --argjson time_sec "$TIME" \
        --argjson table_size "$TABLE_SIZE" \
        --argjson tables "$TABLES" \
        --argjson results "$json_results" \
        '{
            "timestamp": $ts,
            "environment": {
                "threads": $threads,
                "time_seconds": $time_sec,
                "table_size": $table_size,
                "tables": $tables
            },
            "results": $results,
            "targets": {
                "max_p95_overhead_percent": 10.0,
                "min_tps": 1000
            }
        }')

    echo "$final_json" > "$RESULTS_DIR/latest.json"
    echo ""
    echo "=== JSON 결과 저장: $RESULTS_DIR/latest.json ==="

    # 10. 콘솔 비교 테이블
    echo ""
    echo "=========================================="
    echo " 벤치마크 결과 비교"
    echo "=========================================="
    printf "%-20s | %-8s | %-8s | %-10s | %-10s | %-10s | %-12s | %-12s\n" \
        "워크로드" "모드" "TPS" "QPS" "Avg(ms)" "P95(ms)" "P99(ms)" "오버헤드"
    printf -- "%.0s-" {1..110}
    echo ""

    for workload in "${WORKLOAD_LIST[@]}"; do
        local d_tps d_qps d_avg d_p95 d_p99
        d_tps=$(echo "$final_json" | jq -r ".results.\"$workload\".direct.tps")
        d_qps=$(echo "$final_json" | jq -r ".results.\"$workload\".direct.qps")
        d_avg=$(echo "$final_json" | jq -r ".results.\"$workload\".direct.latency_avg_ms")
        d_p95=$(echo "$final_json" | jq -r ".results.\"$workload\".direct.latency_p95_ms")
        d_p99=$(echo "$final_json" | jq -r ".results.\"$workload\".direct.latency_p99_ms")

        local p_tps p_qps p_avg p_p95 p_p99
        p_tps=$(echo "$final_json" | jq -r ".results.\"$workload\".proxy.tps")
        p_qps=$(echo "$final_json" | jq -r ".results.\"$workload\".proxy.qps")
        p_avg=$(echo "$final_json" | jq -r ".results.\"$workload\".proxy.latency_avg_ms")
        p_p95=$(echo "$final_json" | jq -r ".results.\"$workload\".proxy.latency_p95_ms")
        p_p99=$(echo "$final_json" | jq -r ".results.\"$workload\".proxy.latency_p99_ms")

        local oh_p95
        oh_p95=$(echo "$final_json" | jq -r ".results.\"$workload\".overhead.latency_p95_percent")

        printf "%-20s | %-8s | %8s | %10s | %10s | %10s | %12s | %12s\n" \
            "$workload" "direct" "$d_tps" "$d_qps" "$d_avg" "$d_p95" "$d_p99" "-"
        printf "%-20s | %-8s | %8s | %10s | %10s | %10s | %12s | %+11.1f%%\n" \
            "" "proxy" "$p_tps" "$p_qps" "$p_avg" "$p_p95" "$p_p99" "$oh_p95"
        printf -- "%.0s-" {1..110}
        echo ""
    done

    # 11. 성능 목표 PASS/FAIL
    echo ""
    echo "=== 성능 목표 확인 ==="
    local all_pass=true

    for workload in "${WORKLOAD_LIST[@]}"; do
        local oh_p95 p_tps
        oh_p95=$(echo "$final_json" | jq -r ".results.\"$workload\".overhead.latency_p95_percent")
        p_tps=$(echo "$final_json" | jq -r ".results.\"$workload\".proxy.tps")

        # P95 오버헤드 <= 10%
        if (( $(echo "$oh_p95 <= 10.0" | bc -l) )); then
            echo "  PASS  $workload P95 오버헤드: ${oh_p95}% (<= 10%)"
        else
            echo "  FAIL  $workload P95 오버헤드: ${oh_p95}% (> 10%)"
            all_pass=false
        fi

        # TPS >= 1000
        if (( $(echo "$p_tps >= 1000" | bc -l) )); then
            echo "  PASS  $workload 프록시 TPS: ${p_tps} (>= 1000)"
        else
            echo "  FAIL  $workload 프록시 TPS: ${p_tps} (< 1000)"
            all_pass=false
        fi
    done

    echo ""
    if [[ "$all_pass" == true ]]; then
        echo "=========================================="
        echo " 모든 성능 목표 달성 (PASS)"
        echo "=========================================="
    else
        echo "=========================================="
        echo " 일부 성능 목표 미달 (FAIL)"
        echo "=========================================="
    fi
}

main "$@"
