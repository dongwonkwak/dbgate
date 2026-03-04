# dbgate 운영 런북 (Runbook)

## 목적
- 운영자/개발자가 `dbgate`를 실행, 점검, 장애 대응, 롤백할 때 필요한 절차를 정리한다.
- 다른 AI/자동화 도구가 운영 절차를 이해할 수 있도록 단계별 입력/출력을 명확히 한다.

## 적용 범위
- 로컬 개발 환경
- 통합 테스트 환경
- (추후) 실제 운영 환경

## 관련 문서
- `README.md`
- `docs/architecture.md`
- `docs/uds-protocol.md`
- `docs/observability.md` (작성 예정/존재 시)
- `docs/failure-modes.md` (작성 예정/존재 시)

## 운영 원칙
1. 재현 가능한 명령을 남긴다.
2. 설정/정책 변경 시 롤백 방법을 함께 기록한다.
3. 장애 대응 절차는 "관측 -> 진단 -> 조치 -> 검증" 순서를 따른다.
4. 문서와 실제 명령/경로가 불일치하면 문서를 먼저 수정한다.

## 빠른 시작 체크리스트 (초안)
- [ ] 필요한 의존성/컨테이너 준비
- [ ] 설정 파일 위치 확인 (`config/`, `docker-compose.yaml`)
- [ ] `dbgate` 실행
- [ ] 헬스체크/로그/통계 확인
- [ ] 테스트 쿼리(정상/차단) 수행

## 운영 절차 템플릿

### 작업 이름
- 목적:
- 사전 조건:
- 입력:
- 수행 명령:
- 기대 결과:
- 실패 시 확인 포인트:
- 롤백 절차:

## 절차 1: 서비스 기동 (초안)
- 목적: 로컬 또는 통합 환경에서 `dbgate`를 기동한다.
- 사전 조건: 설정/정책 파일 준비, 포트 충돌 없음
- 입력: 실행 환경(로컬/컨테이너)
- 수행 명령: TODO (프로젝트 실제 명령으로 채울 것)
- 기대 결과: 프로세스/컨테이너 기동, 헬스체크 정상, 로그 초기화 확인
- 실패 시 확인 포인트:
  - 포트 충돌
  - 정책 파일 로드 실패
  - 업스트림 MySQL 연결 실패
- 롤백 절차: 기동 중단 후 이전 설정/이미지/compose 상태 복원

## 절차 2: 정책 변경 반영 (초안)
- 목적: 정책 파일 변경 후 동작 반영 여부를 검증한다.
- 사전 조건: 현재 정책 백업 확보
- 입력: 새 정책 파일
- 수행 명령:
  ```bash
  # 정책 리로드 (버전 번호 및 적용된 룰 수 출력)
  dbgate-cli policy reload
  # → Policy reloaded successfully (version 6)
  # → Rules count: 24

  # 현재 버전 및 이력 확인
  dbgate-cli policy versions
  # → Current version: 6
  # → === Policy Versions ===
  # → Version  Timestamp                  Rules  Hash
  # → 6        2026-03-04T10:45:00Z       24     abc12345...
  # → 5        2026-03-04T10:30:00Z       22     def67890...

  # 특정 버전으로 롤백
  dbgate-cli policy rollback --version 5
  # → Rolled back to version 5 (from version 6)
  # → Rules count: 22
  ```
- 기대 결과: 정책 리로드 성공 메시지(버전 번호 포함), 테스트 쿼리 동작 변경 확인
- 실패 시 확인 포인트:
  - YAML 파싱 오류
  - 리로드 실패 응답/로그
  - fail-close 동작 여부
- 롤백 절차: `dbgate-cli policy rollback --version <이전버전>` 실행

## 절차 3: 장애 대응 (초안 템플릿)
- 증상 예시:
  - 연결 실패 증가
  - 쿼리 차단율 급증
  - UDS 조회 실패
- 1차 확인:
  - 프로세스/컨테이너 상태
  - 헬스체크 상태
  - 최근 에러 로그
  - stats/세션 지표
- 조치:
  - 설정/정책 롤백
  - 업스트림 DB 상태 확인
  - 필요 시 재시작
- 검증:
  - 정상 쿼리/차단 쿼리 시나리오 재실행

## 운영 커맨드 표

### 빌드

| 목적 | 명령 | 비고 |
|---|---|---|
| 기본 빌드 | `cmake --preset default && cmake --build build/default` | Release |
| 디버그 빌드 | `cmake --preset debug && cmake --build build/debug` | |
| ASan 빌드 | `cmake --preset asan && cmake --build build/asan` | 메모리 오류 탐지 |
| TSan 빌드 | `cmake --preset tsan && cmake --build build/tsan` | 데이터레이스 탐지 |
| 테스트 실행 | `cmake --build build/default --target test` | 전체 단위 테스트 322개 |

### 환경변수 기반 설정 (Docker/로컬)

| 환경변수 | 기본값 | 설명 |
|---|---|---|
| `PROXY_LISTEN_ADDR` | `0.0.0.0` | 프록시 리슨 주소 |
| `PROXY_LISTEN_PORT` | `13306` | 프록시 리슨 포트 |
| `MYSQL_HOST` | `127.0.0.1` | 업스트림 MySQL 호스트 |
| `MYSQL_PORT` | `3306` | 업스트림 MySQL 포트 |
| `POLICY_PATH` | `config/policy.yaml` | 정책 파일 경로 |
| `UDS_SOCKET_PATH` | `/tmp/dbgate.sock` | Go 운영도구 UDS 소켓 경로 |
| `LOG_PATH` | `/tmp/dbgate.log` | 로그 파일 경로 |
| `LOG_LEVEL` | `info` | 로그 레벨 (trace/debug/info/warn/error) |
| `HEALTH_CHECK_PORT` | `8080` | 헬스체크 HTTP 포트 |
| `MAX_CONNECTIONS` | `1000` | 최대 동시 연결 수 |
| `CONNECTION_TIMEOUT_SEC` | `30` | 세션 유휴 타임아웃(초) |

### UDS 통계 조회 (수동)

```bash
# stats 커맨드: 4바이트 LE 헤더 + JSON 요청
REQUEST='{"command":"stats","version":1}'
LEN=$(printf '%s' "$REQUEST" | wc -c)
HEADER=$(python3 -c "import struct,sys; sys.stdout.buffer.write(struct.pack('<I', $LEN))")
(printf '%b' "$HEADER"; printf '%s' "$REQUEST") | nc -U /tmp/dbgate.sock | tail -c +5
```

기대 응답 예:
```json
{"ok":true,"payload":{"total_connections":42,"active_sessions":3,"total_queries":1250,"blocked_queries":15,"qps":25.5000,"block_rate":0.0120,"captured_at_ms":1740218645123}}
```

### 정책 핫 리로드 (SIGHUP)

```bash
# dbgate 프로세스 PID 확인 후 SIGHUP 전송
kill -HUP $(pgrep dbgate)
# 로그에서 확인: [proxy] policy reloaded successfully
```

### 시그널 기반 제어

| 시그널 | 동작 |
|---|---|
| `SIGTERM` | Graceful Shutdown (활성 세션 완료 후 종료) |
| `SIGINT` | Graceful Shutdown |
| `SIGHUP` | 정책 파일 핫 리로드 (기존 정책 유지하며 재로드) |

## CI/CD 파이프라인

### GitHub Actions 워크플로우

| 워크플로우 | 파일 | 트리거 경로 | 비고 |
|---|---|---|---|
| C++ CI | `.github/workflows/ci.yml` | `src/**`, `tests/**`, `CMakeLists.txt` | 4개 job: 빌드/clang-tidy/ASan/TSan |
| Go CI | `.github/workflows/go.yml` | `tools/**`, `.golangci.yml` | golangci-lint + go test -race |
| Commit/PR Lint | `.github/workflows/lint.yml` | PR 이벤트 | 커밋 메시지·PR 제목 형식 검증 |
| Integration | `.github/workflows/integration.yml` | `src/**`, `tests/integration/**` | docker-compose + MySQL 연동 |
| Docs Impact | `.github/workflows/docs-impact-check.yml` | PR 이벤트 | 문서 영향도 자동 검증 |

### CI 캐시 전략

vcpkg 의존성 컴파일 시간 절감을 위해 바이너리 캐시를 사용한다:
- 캐시 키: `vcpkg-linux-x64-<vcpkg.json 해시>`
- 캐시 경로: `/tmp/vcpkg-bincache`
- 첫 실행 시 15~30분 소요, 이후 캐시 히트 시 1~2분

### CI 실패 시 대응

| 실패 job | 원인 | 조치 |
|---|---|---|
| Build & Test | 빌드 오류 또는 테스트 실패 | 로컬 `cmake --preset default && ctest` 재현 |
| Static Analysis | clang-tidy error 또는 cppcheck 오류 | `clang-tidy -p build/debug <파일>.cpp` 로컬 실행 |
| ASan | 메모리 오염/누수 | `cmake --preset asan && ctest` 로컬 실행 |
| TSan | 데이터레이스 | `cmake --preset tsan && ctest` 로컬 실행 |
| Go CI | 린트 오류 또는 테스트 실패 | `cd tools && golangci-lint run` 로컬 실행 |
| Commit/PR Lint | 메시지 형식 오류 | `type(scope): 설명 [DON-XX]` 형식 준수 |

### TSan runner 제약
TSan job은 `ubuntu-24.04` (x86_64 고정) 에서만 실행한다.
GCC ThreadSanitizer가 aarch64에서 불안정하므로 runner 아키텍처를 명시적으로 제한한다.

## Docker 배포 (멀티 인스턴스 + HAProxy)

### 아키텍처 개요

```
Client ──► HAProxy(:13306) ──┬── dbgate-1(:13306) ──► MySQL(:3306)
           stats(:8404)      ├── dbgate-2(:13306) ──►
                             └── dbgate-3(:13306) ──►
```

- HAProxy가 TCP(L4) 라운드로빈으로 dbgate 3대에 분배
- 각 dbgate 인스턴스는 HTTP 헬스체크(`:8080/health`)를 제공
- 정책 파일(`config/`)은 bind mount로 읽기 전용 공유

### 이미지 빌드

```bash
# C++ 코어 이미지
docker build -f deploy/Dockerfile -t dbgate:latest .

# Go CLI 이미지
docker build -f deploy/Dockerfile.tools -t dbgate-tools:latest .
```

### 전체 기동

```bash
cd deploy
cp .env.example .env   # 시크릿 값 반드시 수정
# .env 파일에서 MYSQL_ROOT_PASSWORD, MYSQL_PASSWORD, HAPROXY_STATS_AUTH 변경
docker compose up --build -d
```

기동 순서: MySQL → dbgate-1/2/3 (MySQL healthy 대기) → HAProxy (dbgate healthy 대기)

### 기동 확인

```bash
# HAProxy 경유 MySQL 접속
mysql -h 127.0.0.1 -P 13306 -u dbgate -p dbgate_test

# HAProxy 통계 페이지 (인증 필요, .env의 HAPROXY_STATS_AUTH 참조)
curl -u admin:changeme_stats http://localhost:8404/stats

# 개별 인스턴스 헬스체크
docker compose exec dbgate-1 curl -s http://localhost:8080/health
# → {"status":"ok"}
```

### 서비스 중지

```bash
cd deploy

# 정상 종료 (graceful, 30초 대기)
docker compose down

# 볼륨 포함 전체 삭제
docker compose down -v
```

### 정책 변경 반영

정책 파일은 `config/` 디렉토리에서 bind mount되므로 호스트에서 수정 후 리로드:

```bash
# 정책 파일 수정
vi config/policy.yaml

# 전체 인스턴스에 SIGHUP 전송
docker compose kill -s HUP dbgate-1 dbgate-2 dbgate-3

# 로그에서 리로드 확인
docker compose logs --tail=5 dbgate-1
# → [proxy] policy reloaded successfully
```

### 로그 확인

```bash
# 전체 로그 (팔로우)
docker compose logs -f

# 특정 인스턴스 로그
docker compose logs -f dbgate-2

# HAProxy 로그
docker compose logs -f haproxy
```

### 모니터링

| 대상 | 엔드포인트 | 설명 |
|------|-----------|------|
| HAProxy 통계 | `http://localhost:8404/stats` | 백엔드 상태, 연결 수, 에러율 |
| dbgate 대시보드 | `http://localhost:8081` | 실시간 QPS, 차단율, 세션 현황 |
| dbgate 헬스체크 | `http://dbgate-N:8080/health` | 인스턴스별 상태 |
| UDS 통계 | 컨테이너 내부 `/run/dbgate/dbgate.sock` | 세션/쿼리/차단 지표 |

### 스케일 변경

인스턴스 수를 변경하려면:

1. `deploy/docker-compose.yaml`에 `dbgate-N` 서비스 추가/삭제
2. `deploy/haproxy.cfg`의 `backend dbgate_backends`에 서버 추가/삭제
3. `docker compose up --build -d`로 재배포

### 장애 대응

| 증상 | 확인 | 조치 |
|------|------|------|
| HAProxy stats에서 backend DOWN | `docker compose ps`, 개별 헬스체크 | 해당 인스턴스 로그 확인 → 재시작 |
| 전체 연결 불가 | MySQL 상태, HAProxy 로그 | MySQL 헬스체크 → `docker compose restart mysql` |
| 정책 리로드 실패 | dbgate 로그에서 YAML 파싱 오류 | 정책 파일 문법 확인 → 이전 버전 복원 → 재리로드 |
| 높은 지연시간 | HAProxy stats의 큐 깊이 | 인스턴스 추가 또는 `MAX_CONNECTIONS` 조정 |

### 환경변수 참조

docker-compose.yaml에서 오버라이드 가능한 주요 환경변수:

| 환경변수 | 기본값 | 설명 |
|---|---|---|
| `MYSQL_HOST` | `127.0.0.1` | 업스트림 MySQL 호스트 |
| `MYSQL_PORT` | `3306` | 업스트림 MySQL 포트 |
| `PROXY_LISTEN_PORT` | `13306` | 프록시 리슨 포트 |
| `HEALTH_CHECK_PORT` | `8080` | 헬스체크 HTTP 포트 |
| `POLICY_PATH` | `/etc/dbgate/policy.yaml` | 정책 파일 경로 |
| `LOG_LEVEL` | `info` | 로그 레벨 |
| `MAX_CONNECTIONS` | `1000` | 최대 동시 연결 수 |
| `CONNECTION_TIMEOUT_SEC` | `30` | 세션 유휴 타임아웃(초) |

전체 환경변수 목록은 [환경변수 기반 설정](#환경변수-기반-설정-docker로컬) 섹션 참조.

## 웹 대시보드 (dbgate-dashboard)

### 개요

`dbgate-dashboard`는 Go + htmx 기반의 웹 대시보드로, C++ 코어의 UDS 통계를 실시간으로 시각화한다.

- 기본 포트: `:8081` (내부 전용, `127.0.0.1` 바인딩)
- 갱신 주기: 통계 2초, 세션 5초 (htmx polling)
- 외부 의존성 없음: htmx, Pico CSS를 바이너리에 내장 (에어갭 환경 대응)

### 기동

```bash
# 바이너리 직접 실행
dbgate-dashboard --socket /tmp/dbgate.sock --listen :8081

# Docker Compose 사용
cd deploy
docker compose up -d dbgate-dashboard
```

### 플래그

| 플래그 | 기본값 | 설명 |
|--------|--------|------|
| `--listen` | `:8081` | HTTP 리슨 주소 |
| `--socket` | `/tmp/dbgate.sock` | dbgate UDS 소켓 경로 |
| `--timeout` | `5s` | UDS 요청 타임아웃 |

### 접속 확인

```bash
# 대시보드 페이지 확인
curl -s http://localhost:8081/ | head -20

# 통계 API 직접 호출
curl -s http://localhost:8081/api/stats
```

### 엔드포인트

| 경로 | 설명 |
|------|------|
| `GET /` | 메인 대시보드 페이지 |
| `GET /api/stats` | 통계 HTML 프래그먼트 (htmx partial) |
| `GET /api/sessions` | 세션 HTML 프래그먼트 |
| `GET /api/chart-data` | QPS 차트 데이터 (JSON via script tag) |
| `GET /api/policy-versions` | 정책 버전 히스토리 HTML 프래그먼트 (htmx partial) |
| `POST /api/policy-rollback` | 특정 버전으로 정책 롤백 (htmx partial 반환) |
| `GET /static/*` | 정적 에셋 (htmx.min.js, pico.min.css, dashboard.js) |
| `GET /policy-tester` | Policy Tester 페이지 (SQL dry-run) |
| `POST /api/policy-explain` | 정책 explain 결과 (htmx partial) |

### Policy Tester 보안 고려사항

`/policy-tester` 및 `POST /api/policy-explain` 응답에는 `evaluation_path`와 `matched_access_rule` 필드가 포함된다.
이 정보는 정책 매칭 경로와 접근 규칙 구조를 노출하므로 아래 통제를 반드시 적용한다.

**UDS 소켓 파일 권한**

```bash
# dbgate 프로세스 기동 후 소켓 권한 확인 (dbgate 사용자만 읽기/쓰기)
ls -la /tmp/dbgate.sock
# 예상: srwx------ 1 dbgate dbgate ...

# 권한이 넓으면 수동 조정
chmod 700 /tmp/dbgate.sock
```

> UDS 소켓에 접근할 수 있으면 `policy_explain`을 직접 호출하여 정책 구조를 파악할 수 있다.
> 소켓 파일은 dbgate 프로세스 사용자 소유로 제한하고, 대시보드 프로세스 사용자에게만 읽기 권한을 부여한다.

**대시보드 접근 통제**

- Basic Auth(`--auth user:password` 플래그 또는 `DASHBOARD_AUTH` 환경변수) 미설정 시 서버 기동 실패
- 운영 환경에서는 반드시 리버스 프록시(nginx, Caddy 등) 뒤에 배치하거나 VPN/내부망 전용으로 제한
- `--listen` 기본값은 `:8081`(와일드카드) — Docker 없이 직접 실행 시 `127.0.0.1:8081`로 명시 권장

```bash
# 권장 기동 방식 (localhost 바인딩 + Basic Auth)
dbgate-dashboard --socket /tmp/dbgate.sock --listen 127.0.0.1:8081 --auth admin:강한패스워드
```

### 트러블슈팅

| 증상 | 원인 | 조치 |
|------|------|------|
| "Failed to fetch statistics" 표시 | UDS 소켓 연결 실패 | dbgate 인스턴스 상태 확인, `--socket` 경로 확인 |
| "Coming Soon" (세션 섹션) | C++ 측 sessions 명령 미구현 | 정상 동작 — C++ 구현 (Phase 3 예정) 후 자동 표시 |
| 정책 버전 목록 미표시 또는 오류 | C++ 측 policy_versions 미주입 | dbgate가 version_store 없이 기동된 경우. 설정 파일에서 정책 경로 확인 후 재시작 (DON-50 완료) |
| 롤백 버튼 클릭 후 오류 표시 | 대상 버전 없음 또는 policy_rollback 미구현 | 오류 메시지 확인, `dbgate-cli policy versions`로 유효 버전 목록 조회. DON-50 완료 후 정상 동작 |
| 페이지 접속 불가 | 대시보드 미기동 또는 포트 충돌 | `docker compose ps`, 포트 확인 |
| 차트 데이터 없음 | 대시보드 기동 직후 | 통계 수집 후 자동 표시 (최대 2분 히스토리) |

### 중지

```bash
# Docker Compose
docker compose stop dbgate-dashboard

# 바이너리 직접 실행 시
# SIGTERM 또는 SIGINT (Ctrl+C) → graceful shutdown
kill -TERM $(pgrep dbgate-dashboard)
```

## 변경 체크리스트 (문서 유지보수용)
- CLI 명령/옵션이 실제 `tools/` 구현과 일치하는가?
- 배포/기동/롤백 절차가 실제 환경과 일치하는가?
- 관측성 문서(`docs/observability.md`)와 지표/로그 항목이 일치하는가?
- CI 워크플로우 변경 시 이 섹션의 표를 업데이트했는가?
