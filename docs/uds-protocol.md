# Unix Domain Socket (UDS) 프로토콜 명세

## 개요

dbgate C++ 코어와 Go CLI/대시보드 도구 간의 **저오버헤드 통신** 메커니즘입니다.

**통신 방식**: Unix Domain Socket (AF_UNIX)
**전송 방식**: 길이 프리픽스 + JSON 페이로드
**프로토콜 버전**: 1 (현재)

---

## 프레임 포맷

### 기본 구조

```
[4byte LE length][JSON body]
```

모든 요청 및 응답은 위 포맷을 따릅니다.

#### 길이 필드 (4byte, Little-Endian)

- **범위**: 0 ~ 4,294,967,295 (uint32_t)
- **순서**: Little-Endian (Intel x86/x64 표준)
- **의미**: 뒤따르는 JSON body의 **바이트 길이** (4byte 헤더 제외)

**예시**:
```
Raw Bytes: 0x1D 0x00 0x00 0x00
Decoded:   29 (10진수)
Meaning:   JSON body는 29 바이트
```

#### JSON Body

- **인코딩**: UTF-8 (null-terminated 아님)
- **포맷**: 공백 없는 compact JSON (대역폭 절감)
- **예시**: `{"command":"stats","version":1}`

### 완전한 프레임 예

```
길이 헤더      JSON Body
[0x20 0x00 ... {"command":"stats","version":1}
 00 00]
│─4바이트─│                    │─32바이트────────────────│
```

---

## 요청 형식

### CommandRequest

클라이언트(Go CLI)가 서버(C++ UdsServer)로 보내는 명령입니다.

```json
{
  "command": "stats",
  "version": 1
}
```

#### 필드

| 필드 | 타입 | 필수 | 기본값 | 설명 |
|------|------|------|--------|------|
| `command` | string | ✓ | - | 실행할 커맨드 이름 |
| `version` | int | ✗ | 1 | 프로토콜 버전 (향후 호환성용) |

#### 지원 커맨드

##### 1. stats

실시간 통계 스냅샷을 조회합니다.

**요청**:
```json
{
  "command": "stats",
  "version": 1
}
```

**응답** (성공):
```json
{
  "ok": true,
  "payload": {
    "total_connections": 42,
    "active_sessions": 3,
    "total_queries": 1250,
    "blocked_queries": 15,
    "qps": 25.5,
    "block_rate": 0.012,
    "captured_at_ms": 1740218645123
  }
}
```

**응답** (실패):
```json
{
  "ok": false,
  "error": "Internal statistics error"
}
```

**용도**:
- 실시간 모니터링 대시보드
- CLI 통계 조회
- 알림 시스템 (QPS/차단율 임계값 확인)

---

##### 2. sessions

활성 세션 목록을 조회합니다. (Phase 3 구현 예정)

**요청**:
```json
{
  "command": "sessions",
  "version": 1
}
```

**응답** (성공, Phase 3):
```json
{
  "ok": true,
  "payload": [
    {
      "session_id": 1,
      "client_ip": "192.168.1.100",
      "client_port": 54321,
      "db_user": "app_service",
      "db_name": "production",
      "connected_at": "2026-02-22T10:25:00Z",
      "state": "ready"
    },
    {
      "session_id": 2,
      "client_ip": "192.168.1.101",
      "client_port": 54322,
      "db_user": "readonly_user",
      "db_name": "analytics",
      "connected_at": "2026-02-22T10:26:30Z",
      "state": "processing_query"
    }
  ]
}
```

**용도**:
- 현재 활성 연결 모니터링
- 특정 세션 강제 종료 (향후 확장)
- 디버깅 및 감시

---

##### 3. policy_reload

정책 설정 파일을 리로드합니다. (Phase 3 구현 예정)

**요청**:
```json
{
  "command": "policy_reload",
  "version": 1
}
```

**응답** (성공):
```json
{
  "ok": true,
  "payload": {
    "reloaded_at": "2026-02-22T10:35:20Z",
    "rules_count": 24,
    "message": "Policy reloaded successfully"
  }
}
```

**응답** (실패):
```json
{
  "ok": false,
  "error": "Invalid YAML syntax in policy file"
}
```

**용도**:
- 정책 파일 변경 후 수동 리로드 (Hot Reload 보완)
- 설정 적용 실패 시 명시적 재시도
- 운영 편의성

---

## 응답 형식

### Response (공통 래퍼)

모든 응답은 다음 포맷을 따릅니다:

```json
{
  "ok": true/false,
  "error": "...",
  "payload": {...}
}
```

#### 필드

| 필드 | 타입 | 조건 | 설명 |
|------|------|------|------|
| `ok` | bool | ✓ | 요청 성공 여부 |
| `error` | string | `ok=false` | 오류 메시지 (성공 시 생략 가능) |
| `payload` | object/array | `ok=true` | 결과 데이터 |

#### 성공 응답

```json
{
  "ok": true,
  "payload": { ... }
}
```

**특징**:
- `ok=true`
- `error` 필드 생략 가능
- `payload` 필드 포함 (커맨드별 형식)

#### 실패 응답

```json
{
  "ok": false,
  "error": "Descriptive error message"
}
```

**특징**:
- `ok=false`
- `error` 필드 **필수**
- `payload` 필드 생략

---

## StatsSnapshot 상세

`stats` 커맨드의 응답 payload 필드입니다.

```json
{
  "total_connections": 42,
  "active_sessions": 3,
  "total_queries": 1250,
  "blocked_queries": 15,
  "qps": 25.5,
  "block_rate": 0.012,
  "captured_at_ms": 1740218645123
}
```

### 필드 설명

| 필드 | 타입 | 설명 |
|------|------|------|
| `total_connections` | uint64 | 프로세스 시작 이후 누적 연결 수 |
| `active_sessions` | uint64 | 현재 활성 세션 수 |
| `total_queries` | uint64 | 누적 쿼리 처리 수 (ALLOW + BLOCK) |
| `blocked_queries` | uint64 | 정책에 의해 차단된 쿼리 수 |
| `qps` | double | 1초 슬라이딩 윈도우 기반 초당 쿼리 수 |
| `block_rate` | double | 차단 비율 (0.0 ~ 1.0), `blocked_queries / total_queries` |
| `captured_at_ms` | int64 | 스냅샷 생성 시각 (Unix epoch 밀리초) |

### 계산식

```
block_rate = (total_queries > 0) ? blocked_queries / total_queries : 0.0
qps = window_queries / elapsed_seconds (1초 슬라이딩 윈도우)
```

---

## 클라이언트 구현 예시

### Go (tools/internal/client/types.go)

```go
package client

import (
    "time"
)

// StatsSnapshot은 C++ StatsCollector.snapshot()의 JSON 표현
type StatsSnapshot struct {
    TotalConnections uint64    `json:"total_connections"`
    ActiveSessions   uint64    `json:"active_sessions"`
    TotalQueries     uint64    `json:"total_queries"`
    BlockedQueries   uint64    `json:"blocked_queries"`
    QPS              float64   `json:"qps"`
    BlockRate        float64   `json:"block_rate"`
    CapturedAt       time.Time `json:"captured_at"`
}

// CommandRequest는 C++ dbgate core로 송신하는 요청
type CommandRequest struct {
    Command string `json:"command"`           // "stats" | "sessions" | "policy_reload"
    Version int    `json:"version,omitempty"` // 기본값 1
}

// Response는 C++ dbgate core의 응답 래퍼
type Response struct {
    OK      bool        `json:"ok"`
    Error   string      `json:"error,omitempty"`
    Payload interface{} `json:"payload,omitempty"`
}
```

### C++ 클라이언트 (향후 예시)

```cpp
// Go도구가 이 인터페이스로 요청
std::vector<uint8_t> request_frame;

// 1. JSON 직렬화
std::string json_body = R"({"command":"stats","version":1})";

// 2. 길이 헤더 작성 (Little-Endian, uint32_t)
uint32_t len = json_body.size();
request_frame.push_back((len >>  0) & 0xFF);
request_frame.push_back((len >>  8) & 0xFF);
request_frame.push_back((len >> 16) & 0xFF);
request_frame.push_back((len >> 24) & 0xFF);

// 3. JSON body 추가
request_frame.insert(request_frame.end(),
                     json_body.begin(),
                     json_body.end());

// 4. UDS 소켓으로 송신
co_await socket.async_write_some(asio::buffer(request_frame));

// 5. 응답 수신 및 파싱
std::vector<uint8_t> header(4);
co_await socket.async_read_exact(asio::buffer(header));
uint32_t response_len =
    (header[0] <<  0) |
    (header[1] <<  8) |
    (header[2] << 16) |
    (header[3] << 24);

std::vector<uint8_t> response_body(response_len);
co_await socket.async_read_exact(asio::buffer(response_body));

std::string json_response(response_body.begin(), response_body.end());
// JSON 파싱...
```

---

## 통신 시나리오

### 시나리오 1: Stats 조회 (정상)

```
Go CLI                              C++ UdsServer
   │                                    │
   ├─ 요청 송신 ─────────────────────────→
   │  [0x1D 0x00 0x00 0x00]
   │  {"command":"stats","version":1}
   │
   │                                  (요청 처리)
   │                          snapshot() 호출
   │                          JSON 직렬화
   │
   │←─ 응답 송신 ─────────────────────────┤
   │  [0xA2 0x00 0x00 0x00]
   │  {"ok":true,"payload":{
   │    "total_connections":42,
   │    "active_sessions":3,
   │    ...
   │  }}
   │
   ├─ 응답 파싱
   ├─ 통계 출력
   └─ 종료
```

### 시나리오 2: Policy Reload (실패)

```
Go CLI                              C++ UdsServer
   │                                    │
   ├─ 요청 송신 ─────────────────────────→
   │  [0x23 0x00 0x00 0x00]
   │  {"command":"policy_reload","version":1}
   │
   │                            (파일 읽기 시도)
   │                               실패
   │                        (YAML 파싱 오류)
   │
   │←─ 오류 응답 ──────────────────────────┤
   │  [0x52 0x00 0x00 0x00]
   │  {"ok":false,"error":"Invalid YAML syntax on line 5"}
   │
   ├─ 오류 처리
   └─ 재시도 프롬프트
```

### 시나리오 3: 프로토콜 버전 호환성

**구버전 클라이언트 (version 미지정)**:
```json
{
  "command": "stats"
}
```

**서버 동작**:
- `version` 필드 없으면 기본값 1로 처리
- 현재 프로토콜 1과 동일하게 응답

**향후 확장 (version 2)**:
```json
{
  "command": "stats",
  "version": 2
}
```

서버는 요청 `version` 필드를 확인하여 호환성 있게 응답합니다.

---

## 에러 처리

### 클라이언트 측 체크리스트

1. **연결 실패**
   ```go
   conn, err := net.Dial("unix", "/tmp/dbgate.sock")
   if err != nil {
       log.Fatalf("UDS 연결 실패: %v", err)
   }
   ```

2. **프레임 파싱 실패**
   ```go
   if len(header) != 4 {
       log.Fatalf("헤더 길이 오류")
   }
   ```

3. **JSON 역직렬화 실패**
   ```go
   var resp Response
   if err := json.Unmarshal(body, &resp); err != nil {
       log.Fatalf("JSON 파싱 실패: %v", err)
   }
   ```

4. **서버 응답 오류**
   ```go
   if !resp.OK {
       log.Fatalf("서버 오류: %s", resp.Error)
   }
   ```

### 서버 측 보장

- **원자성**: 부분 파일 쓰기 방지 (전체 프레임 또는 아무것도)
- **타임아웃**: 느린 클라이언트 연결 제거 (Phase 3)
- **크기 제한**: 메모리 폭발 방지 (max payload 설정)
- **Graceful Shutdown**: 진행 중인 요청 완료 후 종료

---

## 성능 특성

### 대역폭

**stats 응답 예시**:
```
헤더 (4바이트) + JSON (~200바이트) = ~204바이트
```

**비교**:
- HTTP REST: 200+ 바이트 (요청/응답 헤더 포함)
- gRPC: Protocol Buffers (더 작지만 복잡성 증가)
- UDS + JSON: 간단하고 컴팩트

### 레이턴시

**UDS의 장점**:
- TCP 오버헤드 없음 (로컬 커널 IPC)
- 일반적으로 <1ms (로컬 머신)
- 네트워크 지연 없음

### 동시성

**현재** (Phase 2, DON-28 구현):
- Boost.Asio co_await 기반 비동기 accept 루프
- 각 클라이언트는 co_spawn으로 독립 코루틴 처리
- accept 루프와 클라이언트 처리 코루틴이 분리되어 있어 한 클라이언트 오류가 다른 클라이언트에 영향 없음

**Phase 3 확장 예정**:
- sessions 커맨드 구현 (현재 501 placeholder)
- policy_reload 커맨드 구현 (현재 501 placeholder)
- 클라이언트 타임아웃 처리

---

## 보안 고려사항

### 현재 구현

1. **소켓 파일 경로**
   - `/tmp/dbgate.sock` (일반적)
   - 설정 파일에서 지정 가능
   - 파일 권한: 666 (프로세스 사용자 설정)

2. **인증**
   - 없음 (로컬 호스트만 접근 가능)
   - OS 레벨 파일 권한에 의존

3. **암호화**
   - 없음 (로컬 IPC, 평문 가능)

### 향후 보안 강화

- TLS over UDS (비권장, gRPC 고려)
- Unix socket 권한 검사 강화
- 비인가 접근 감시 로깅

---

## 프로토콜 버전 관리

### 현재 버전: 1

**도입 시점**: Phase 2 (인터페이스 정의)

**변경 정책**:
- `version` 필드로 버전 구분
- 하위 호환성 유지 (구버전 클라이언트 지원)
- 필드 추가는 backward-compatible

### 버전 2 (미래)

예상 변경 사항:
- 새 커맨드 추가 (metrics_export 등)
- payload 필드 확장
- 시간 단위 변경 (Unix timestamp → RFC 3339)

**마이그레이션**:
```cpp
// 서버 구현
std::string serialize_response(const Response& resp, int client_version) {
    if (client_version == 1) {
        return serialize_v1(resp);
    } else if (client_version == 2) {
        return serialize_v2(resp);
    } else {
        return serialize_v1(resp);  // 기본값
    }
}
```

---

## 테스트 시나리오

### 단위 테스트 (C++)

```cpp
TEST(UdsServerTest, StatsCommandSuccess) {
    auto uds = UdsServer("/tmp/test.sock", stats_collector, io_ctx);

    std::string request = R"({"command":"stats","version":1})";
    std::vector<uint8_t> frame = create_frame(request);

    // 요청 송신
    socket.send(frame);

    // 응답 수신 및 검증
    auto response = receive_frame(socket);
    auto json = parse_json(response);

    EXPECT_TRUE(json["ok"]);
    EXPECT_GT(json["payload"]["total_connections"], 0);
}
```

### 통합 테스트 (Go)

```go
func TestStatsCommand(t *testing.T) {
    // UDS 소켓 연결
    conn, err := net.Dial("unix", "/tmp/dbgate.sock")
    if err != nil {
        t.Fatalf("연결 실패: %v", err)
    }
    defer conn.Close()

    // 요청 송신
    req := CommandRequest{
        Command: "stats",
        Version: 1,
    }
    body, _ := json.Marshal(req)
    frame := createFrame(body)
    conn.Write(frame)

    // 응답 수신 및 검증
    resp := receiveFrame(conn)
    var result Response
    json.Unmarshal(resp, &result)

    if !result.OK {
        t.Fatalf("서버 오류: %s", result.Error)
    }

    var snapshot StatsSnapshot
    json.Unmarshal(result.Payload, &snapshot)
    if snapshot.ActiveSessions == 0 {
        t.Error("활성 세션 없음")
    }
}
```

---

## 참고 자료

- Go types 정의: `tools/internal/client/types.go`
- C++ 클라이언트: `src/stats/uds_server.hpp`
- 아키텍처: `docs/architecture.md`
- 인터페이스: `docs/interface-reference.md`

## 변경 이력

| 버전 | 날짜 | 변경 사항 |
|------|------|----------|
| 1.0 | 2026-02-22 | 초안 작성 (Phase 2 인터페이스 정의) |
| 1.1 | 2026-02-25 | DON-28: UdsServer 구현 완료. `captured_at` → `captured_at_ms` (Unix epoch ms). 동시성 섹션 업데이트. |
