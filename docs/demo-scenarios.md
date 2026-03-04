# dbgate 데모 시나리오

Docker Compose 기반 실행 가능한 데모 시나리오.
각 시나리오는 독립적으로 실행할 수 있으며, dbgate의 핵심 보안 기능을 검증한다.

## 사전 준비

### 환경 기동

```bash
cd deploy
cp .env.example .env   # 시크릿 값 수정 필수
docker compose up --build -d
```

기동 순서: MySQL → dbgate-1/2/3 → HAProxy → Dashboard

### 기동 확인

```bash
# HAProxy 경유 접속 테스트
mysql -h 127.0.0.1 -P 13306 -u dbgate -p

# 헬스체크 확인
docker compose exec dbgate-1 curl -s http://localhost:8080/health
# → {"status":"ok"}
```

### 기본 정책 구성 (`config/policy.yaml`)

데모에 사용되는 주요 정책 설정:

```yaml
sql_rules:
  block_statements:
    - DROP
    - TRUNCATE
  block_patterns:
    - "UNION\\s+SELECT"
    - "'\\s*OR\\s+'.*'\\s*=\\s*'"
    - "SLEEP\\s*\\("
    - "BENCHMARK\\s*\\("

procedure_control:
  mode: "whitelist"
  whitelist:
    - "sp_get_user_info"
    - "sp_update_order_status"
  block_dynamic_sql: true
  block_create_alter: true

data_protection:
  block_schema_access: true
```

---

## 시나리오 1: 정상 쿼리 허용

**목적**: 허용된 사용자가 허용된 테이블에 SELECT를 실행하면 정상 통과됨을 확인한다.

```bash
# HAProxy 경유 접속 (app_service 사용자 기준)
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e \
  "SELECT * FROM users LIMIT 5;"
```

**기대 결과**: 쿼리 결과가 정상 반환된다.

```bash
# INSERT도 허용됨
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e \
  "INSERT INTO orders (user_id, product_id, quantity) VALUES (1, 100, 2);"
```

**기대 결과**: INSERT가 정상 실행된다 (`app_service`는 SELECT, INSERT, UPDATE 허용).

---

## 시나리오 2: DDL 차단 (DROP TABLE)

**목적**: `block_statements`에 정의된 DDL이 차단됨을 확인한다.

```bash
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e \
  "DROP TABLE users;"
```

**기대 결과**: 연결이 차단되고 에러 응답을 받는다.

```bash
# TRUNCATE도 차단
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e \
  "TRUNCATE TABLE orders;"
```

**기대 결과**: 차단됨.

**로그 확인**:
```bash
docker compose logs --tail=5 dbgate-1
# → "action":"block","matched_rule":"block-statement" 포함 확인
```

---

## 시나리오 3: SQL Injection 차단

### 3.1 Boolean-based Blind Injection

```bash
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e \
  "SELECT * FROM users WHERE name = '' OR '1'='1';"
```

**기대 결과**: `block_patterns`의 `'\\s*OR\\s+'.*'\\s*=\\s*'` 패턴에 매칭되어 차단.

### 3.2 Time-based Blind Injection

```bash
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e \
  "SELECT * FROM users WHERE id = 1 AND SLEEP(5);"
```

**기대 결과**: `SLEEP\\s*\\(` 패턴에 매칭되어 차단.

```bash
# BENCHMARK 변형
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e \
  "SELECT BENCHMARK(10000000, SHA1('test'));"
```

**기대 결과**: `BENCHMARK\\s*\\(` 패턴에 매칭되어 차단.

### 3.3 UNION SELECT Injection

```bash
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e \
  "SELECT * FROM users WHERE id = 1 UNION SELECT * FROM orders;"
```

**기대 결과**: `UNION\\s+SELECT` 패턴에 매칭되어 차단.

**로그 확인**:
```bash
docker compose logs --tail=10 dbgate-1
# → "action":"block","matched_rule":"block-pattern" 포함 확인
```

---

## 시나리오 4: 프로시저 보안

### 4.1 Whitelist 허용

```bash
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e \
  "CALL sp_get_user_info(1);"
```

**기대 결과**: whitelist에 등록된 프로시저이므로 정상 실행.

### 4.2 Whitelist 미등록 차단

```bash
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e \
  "CALL sp_delete_all_data();"
```

**기대 결과**: whitelist에 없는 프로시저이므로 차단.

### 4.3 동적 SQL 차단

```bash
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e \
  "PREPARE stmt FROM 'DROP TABLE users'; EXECUTE stmt;"
```

**기대 결과**: `block_dynamic_sql: true` 설정에 의해 PREPARE 구문 자체가 차단. 추가로 Multi-statement 감지로 이중 차단.

### 4.4 프로시저 생성 차단

```bash
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e \
  "CREATE PROCEDURE sp_evil() BEGIN DROP TABLE users; END;"
```

**기대 결과**: `block_create_alter: true` 설정에 의해 차단.

---

## 시나리오 5: 정책 Hot Reload (SIGHUP)

**목적**: 서비스 재시작 없이 정책 파일을 변경하고 즉시 반영됨을 확인한다.

### Step 1: 현재 정책으로 DDL 차단 확인

```bash
mysql -h 127.0.0.1 -P 13306 -u app_service -p -e "DROP TABLE test_table;"
# → 차단됨
```

### Step 2: 정책 파일 수정

```bash
# block_statements에서 DROP 제거 (테스트 용도)
vi config/policy.yaml
```

`block_statements`에서 `DROP`을 제거하고 저장.

### Step 3: SIGHUP 전송

```bash
# 전체 인스턴스에 SIGHUP 전송
docker compose kill -s HUP dbgate-1 dbgate-2 dbgate-3

# 로그에서 리로드 확인
docker compose logs --tail=3 dbgate-1
# → [proxy] policy reloaded successfully
```

### Step 4: 변경된 정책 확인

```bash
# DROP이 이제 block_statements에서 제거되었으므로...
# (단, blocked_operations에 DROP이 있다면 여전히 차단될 수 있음)
```

### Step 5: 원상 복구

```bash
# 정책 파일 원복 후 다시 SIGHUP
git checkout config/policy.yaml
docker compose kill -s HUP dbgate-1 dbgate-2 dbgate-3
```

> **주의**: Hot Reload 실패 시 기존 정책이 유지된다 (fail-safe). YAML 파싱 오류가 있으면 로그에서 오류를 확인할 수 있다.

---

## 시나리오 6: 벤치마크 실행

**목적**: 직접 연결 대비 프록시 경유 시 성능 오버헤드를 측정한다.

### 전제조건

- `sysbench`, `jq`, `mysql` CLI 설치 필요
- MySQL이 로컬에서 실행 중이어야 함

### 실행

```bash
# 기본 설정 (4 threads, 30초)
./benchmarks/run_benchmark.sh

# 커스텀 설정
./benchmarks/run_benchmark.sh --threads 8 --time 60 --table-size 50000
```

### 옵션

| 옵션 | 기본값 | 설명 |
|------|--------|------|
| `--threads N` | 4 | sysbench 스레드 수 |
| `--time N` | 30 | 실행 시간 (초) |
| `--table-size N` | 10000 | 테이블 행 수 |
| `--tables N` | 4 | 테이블 수 |
| `--skip-build` | - | dbgate 빌드 건너뛰기 |
| `--workloads LIST` | `oltp_read_only,oltp_read_write` | 콤마 구분 워크로드 |

### 성능 목표

| 지표 | 목표 |
|------|------|
| P95 레이턴시 오버헤드 | <= 10% |
| 프록시 TPS | >= 1,000 |

### 결과 확인

```bash
# JSON 결과 파일
cat benchmarks/results/latest.json | jq .

# 콘솔 비교 테이블이 자동 출력됨
```

---

## 시나리오 7: 대시보드 및 CLI 확인

### 7.1 웹 대시보드

```bash
# 브라우저에서 접속
open http://localhost:8081

# 또는 CLI로 확인
curl -s http://localhost:8081/ | head -20
```

**확인 항목**:
- 실시간 QPS 차트
- 차단율 (Block Rate)
- 활성 세션 수
- 총 연결/쿼리/차단 수

### 7.2 통계 API

```bash
# 통계 JSON 조회
curl -s http://localhost:8081/api/stats
```

### 7.3 UDS 직접 통계 조회

```bash
# 컨테이너 내부에서 UDS로 직접 통계 조회
docker compose exec dbgate-1 bash -c '
REQUEST='"'"'{"command":"stats","version":1}'"'"'
LEN=$(printf "%s" "$REQUEST" | wc -c)
HEADER=$(python3 -c "import struct,sys; sys.stdout.buffer.write(struct.pack('<I', $LEN))")
(printf "%b" "$HEADER"; printf "%s" "$REQUEST") | nc -U /run/dbgate/dbgate.sock | tail -c +5
'
```

**기대 응답**:
```json
{
  "ok": true,
  "payload": {
    "total_connections": 42,
    "active_sessions": 3,
    "total_queries": 1250,
    "blocked_queries": 15,
    "qps": 25.5,
    "block_rate": 0.012
  }
}
```

### 7.4 HAProxy 통계

```bash
# HAProxy 통계 페이지 (인증 필요, .env의 HAPROXY_STATS_AUTH 참조)
curl -u admin:changeme_stats http://localhost:8404/stats
```

---

## 정리

```bash
# 전체 서비스 종료
cd deploy
docker compose down

# 볼륨 포함 삭제
docker compose down -v
```

---

## 관련 문서

- [아키텍처 설명서](architecture.md) — 전체 시스템 구조
- [정책 엔진](policy-engine.md) — 정책 평가 파이프라인 상세
- [위협 모델](threat-model.md) — 보안 위협 및 완화 상태
- [운영 런북](runbook.md) — 운영 절차 및 장애 대응
