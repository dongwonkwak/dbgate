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
- 수행 명령: TODO (`dbgate-cli policy reload` 또는 신호 기반 절차)
- 기대 결과: 정책 리로드 성공 로그/응답, 테스트 쿼리 동작 변경 확인
- 실패 시 확인 포인트:
  - YAML 파싱 오류
  - 리로드 실패 응답/로그
  - fail-close 동작 여부
- 롤백 절차: 이전 정책 복원 후 재로드

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

## 변경 체크리스트 (문서 유지보수용)
- CLI 명령/옵션이 실제 `tools/` 구현과 일치하는가?
- 배포/기동/롤백 절차가 실제 환경과 일치하는가?
- 관측성 문서(`docs/observability.md`)와 지표/로그 항목이 일치하는가?
