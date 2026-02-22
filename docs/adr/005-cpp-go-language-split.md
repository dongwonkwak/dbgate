# ADR-005: C++ 데이터패스 + Go 컨트롤플레인 분리

## Status

Accepted

## Context

dbgate는 MySQL 프록시로서 **고성능 SQL 파싱/정책 판정**과 **편리한 운영 도구** 두 가지를 동시에 요구한다. 프로젝트 구현을 위해 다음 세 가지 언어 전략을 검토했다:

### 선택지 1: 전체 Go

모든 기능을 Go로 구현 (프록시, 정책 엔진, CLI, 웹 대시보드).

**성능 특성:**
- 단일 바이너리 배포 가능 → 운영 단순
- Go 표준 라이브러리(net, context, error handling) 일관성
- goroutine 기반 비동기 처리 → 간단한 동시성 모델

**단점:**
- 고트래픽 환경(>1000 qps) 에서 GC stop-the-world 지연 (P99 latency >10ms 관찰)
- 각 요청마다 할당/해제 반복 → 메모리 압박 가중
- CPU-intensive SQL 파싱 시 performance scaling 한계
- WebAssembly 또는 FFI로 C++ 라이브러리 통합 복잡

### 선택지 2: 전체 C++

모든 기능을 C++로 구현 (프록시, 정책 엔진, CLI, 웹 대시보드).

**성능 특성:**
- 고성능: C++23, 링크 타임 최적화, 메모리 풀 활용 → latency <1ms 달성 가능
- 메모리 효율: RAII shared_ptr/unique_ptr → 자동 메모리 관리, 누수 제로
- CPU 효율: 컴파일 최적화 → 소비 전력 최소화

**단점:**
- CLI/웹 대시보드 개발 부담 매우 높음 (C++ 웹 프레임워크 선택지 제한, boost::beast 등 학습곡선 가파름)
- 빌드 시간 증가 (C++ 컴파일 최적화 시간)
- 개발 생산성 저하 (테스트, 배포 반복 주기 길어짐)
- 런타임 에러 감지 어려움 (타입 시스템 강하지만 런타임 유연성 부족)

### 선택지 3: C++ 데이터패스 + Go 컨트롤플레인 분리 (선택)

**데이터패스 (Data Path) - C++23 + Boost.Asio:**
- MySQL 클라이언트 ↔ 서버 중간 릴레이 (핸드셰이크, COM_QUERY 파싱/정책 판정)
- latency-critical, throughput-critical 작업

**컨트롤플레인 (Control Plane) - Go:**
- CLI 도구: `dbgate-cli sessions`, `dbgate-cli policy reload`
- 웹 대시보드: 실시간 통계, 쿼리 로그, 활성 세션 모니터링
- 정책 관리 API
- C++ 코어와 Unix Domain Socket(UDS)으로 통신

**IPC 방식:**
- Unix Domain Socket (로컬 호스트만)
- 프로토콜: 길이 프리픽스 + JSON
- 레이턴시: <1ms (로컬 소켓, TCP 오버헤드 제거)

**성능 특성:**
- 데이터패스 레이턴시: 1-2ms (C++23 + 제로카피 Asio)
- 컨트롤플레인 응답: 10-50ms (Go, 단순 쿼리)
- 메모리 사용: C++ RAII로 제한, 프로토콜 버퍼링 최소화

## Decision

**C++ 데이터패스 + Go 컨트롤플레인**으로 분리한다.

- **C++ 코어** (`src/`): Boost.Asio 프록시 엔진, MySQL 프로토콜 파싱, SQL 파서, 정책 엔진
- **Go 도구** (`tools/`): CLI (cobra), 웹 대시보드 (net/http + htmx), 통계 수집
- **IPC**: Unix Domain Socket, 길이 프리픽스 JSON 프로토콜

각 컴포넌트는 독립 프로세스로 실행되며, C++ 코어는 포트 13306에서 MySQL 프록시 수신, `/run/dbgate.sock`에서 Go 클라이언트 명령 수신.

## Consequences

### Positive

- **레이턴시 제어**: C++23 + Boost.Asio co_await 기반 zero-copy 네트워크 I/O. 각 요청마다 할당 최소화 → latency variance 낮음 (P99 < 5ms). GC stop-the-world 불가능 → predictable 성능.

- **메모리 효율**: RAII shared_ptr/unique_ptr로 자동 메모리 관리. 메모리 풀(object pool) 활용 가능 → 할당/해제 오버헤드 최소. 고트래픽 환경에서 메모리 사용량 안정적.

- **운영 편의성**: Go 표준 라이브러리의 풍부한 생태계 (net/http, context, error handling). 웹 대시보드, CLI 도구 신속 개발 가능. 런타임 성능 덜 중요한 부분은 Go의 생산성 우선.

- **개발 병렬화**: C++ 데이터패스와 Go 컨트롤플레인 개발 독립적. 프로토콜 인터페이스(UDS JSON)만 확정하면 양쪽 병렬 개발 가능.

- **IPC 오버헤드 최소**: Unix Domain Socket → 로컬 호스트 통신만. TCP 포트 할당, 네트워크 스택 오버헤드 제거 → 레이턴시 <1ms. go 관리 요청(statistics, policy reload)은 사용자 입력(사람 속도)이므로 수초 지연 허용 가능.

- **프로덕션 검증성**: 데이터패스는 고성능 언어, 컨트롤플레인은 고생산성 언어 → 각 계층의 강점 활용.

### Negative

- **빌드 복잡도 증가**: 이중 빌드 시스템 (CMake + Go modules). 배포 시 C++ 바이너리 + Go 바이너리 + .so 라이브러리(boost, yaml-cpp) 모두 포함. devcontainer에서 단일 `docker-compose up`은 가능하지만 수동 설치 환경에서는 두 개 설치 + 환경변수 설정 필요.

- **프로토콜 버전 관리 필수**: UDS 상의 JSON 스키마 버전 변경 시 C++ ↔ Go 호환성 유지 필요. 버전 미스매치 → 통신 실패 → 운영자 혼동 가능. 마이그레이션 전략(graceful migration) 필요.

- **배포 복잡도**: 단일 바이너리 아님 → docker-compose/kubernetes manifest에서 두 개 리소스 정의. 버전 관리 복잡 (dbgate-core v1.2.0 + dbgate-tools v1.2.0 동시 배포 필요).

- **디버깅 어려움**: 두 언어 간 통신 문제 발생 시 (UDS 소켓 권한, JSON 파싱 에러) 근본 원인 파악 시간 증가. Go 디버거, C++ 디버거 동시 필요.

- **단일 서버 가정**: UDS는 로컬 호스트만 가능. 원격 대시보드(Go를 분리 서버에 배포) 요구 시 gRPC/HTTP API 추가 필요.

## Alternatives Considered

### 선택지 1: 전체 Go

모든 기능을 Go로 구현하는 단일 언어 전략.

**검토 결과:**
- 배포 단순(단일 바이너리), 개발 생산성 높음
- 하지만 고트래픽 환경(>1000 qps, P99 latency <5ms 요구)에서 GC 영향 관찰됨
- SQL 파싱 + 정책 판정이 CPU-intensive인 경우 고트래픽 환경에서 Go의 GC 지연이 유저 경험에 영향
- 요구사항이 latency-sensitive이므로 부적합

### 선택지 2: 전체 C++

모든 기능을 C++로 구현하는 고성능 전략.

**검토 결과:**
- 최고 성능 달성 가능, 메모리 효율 최고
- 하지만 웹 대시보드(HTTP 프레임워크, 템플릿 엔진, 정적 자산 관리), CLI 도구 개발 부담이 매우 높음
- C++로 데이터 포맷팅, 시간대 파싱(strptime), JSON 생성 반복하기엔 생산성 손실
- 요구사항이 "고성능 SQL 파싱 + 편리한 운영 도구" 둘 다이므로 단일 언어로는 trade-off 불가피
- 개발 일정 준수 어려움

### 선택지 4: 마이크로서비스 (gRPC)

데이터패스(C++)와 컨트롤플레인(Go)을 분리된 서버에서 운영, gRPC로 통신.

**검토 결과:**
- 확장성 최고 (컨트롤플레인 서버 독립 배포 가능)
- 하지만 배포 복잡도 ↑↑↑ (Kubernetes, service mesh 필요)
- gRPC 직렬화 오버헤드 > UDS JSON (네트워크 레이턴시 가중)
- 현 프로젝트 규모(단일 인스턴스 운영)에서 과설계
- 단순성 우선이므로 배제

## Related ADRs

- ADR-001: Boost.Asio로 C++ 비동기 I/O 구현
- ADR-002: 핸드셰이크 패스스루 → C++ 프로토콜 처리
- ADR-003: GCC 14 선택 → C++ 코루틴 안정성

## Implementation Notes

- **IPC 프로토콜 스키마** (길이 프리픽스 JSON):
  ```
  [4byte BE length][JSON payload]

  Request: {"type": "get_stats"}
  Response: {"type": "stats", "qps": 150, "active_sessions": 5}
  ```

- **C++ UDS 서버** (src/stats/uds_server.hpp):
  - Boost.Asio stream_protocol로 Unix Domain Socket 수신
  - JSON 요청 파싱 → nlohmann::json
  - 응답 직렬화 후 send

- **Go UDS 클라이언트** (tools/internal/client/uds.go):
  - `net.Dial("unix", "/run/dbgate.sock")`
  - 길이 프리픽스 읽음 → 요청 전송
  - 응답 읽음 → JSON 파싱

- **호환성 관리**:
  - JSON 스키마에 버전 필드 추가 → 점진적 업그레이드 지원
  - C++/Go 각각 unknown field 무시 → 하위 호환성 보장

- **멀티스레드 안전**:
  - C++: Boost.Asio strand 내에서 UDS 읽음/쓰기 → data race 불가능
  - Go: net.Conn은 동기식, 한 goroutine 할당 권장

## Cost-Benefit Analysis

| 측면 | 비용 | 이득 |
|------|------|------|
| 성능 | 제로 | 데이터패스 C++ → latency <2ms, 메모리 효율 최고 |
| 운영성 | 미미 (배포 2개 바이너리) | Go CLI/대시보드 → 개발 속도 ↑↑ |
| 개발 | UDS 프로토콜 정의, 양쪽 구현 | 병렬 개발 가능, 언어별 강점 활용 |
| 배포 | docker-compose 두 image (경미) | 표준 containerization 가능 |
| 유지보수 | 프로토콜 버전 관리 필수 | 언어별 담당팀 분리 가능 (장기) |

**결론:** 복잡도 약간 증가, 성능 + 생산성 모두 최적화 → 채택.
