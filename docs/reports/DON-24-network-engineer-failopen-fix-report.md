# DON-24 네트워크 엔지니어 완료 보고서 — fail-open 수정

**작성일**: 2026-02-24
**담당**: network-engineer
**브랜치**: `feat/DON-24-mysql-protocol`
**작업 ID**: DON-24 (feat: extract_handshake_response_fields fail-open 2건 수정)

---

## 1. 변경 파일 목록

| 파일 | 변경 유형 | 내용 |
|------|---------|------|
| `/workspace/src/protocol/handshake.cpp` | `behavior` (버그 수정) | fail-open 3개소 → fail-close 변환 |
| `/workspace/tests/test_handshake_auth.cpp` | `behavior` (테스트 추가 + 기존 버그 수정) | 신규 테스트 2건 추가, 기존 테스트 helper 버그 수정 |

---

## 2. 변경 요약

### 2.1 구현 수정 — src/protocol/handshake.cpp

#### Major 1-A: CLIENT_PLUGIN_AUTH_LENENC 분기 fail-open 제거 (line 471-479)

**수정 전 (fail-open)**:
```cpp
if (pos >= payload.size()) {
    // auth_response 길이 바이트 없음 — 빈 auth_response로 허용
    // (일부 클라이언트는 빈 auth_response를 보낼 수 있음)
    out_db.clear();
    return std::expected<void, ParseError>{};
}
```

**수정 후 (fail-close)**:
```cpp
if (pos >= payload.size()) {
    return std::unexpected(ParseError{
        ParseErrorCode::kMalformedPacket,
        "auth_response length prefix missing",
        std::format("pos={}, payload_size={}", pos, payload.size())
    });
}
```

#### Major 1-B: CLIENT_SECURE_CONNECTION 분기 fail-open 제거 (line 532-540)

**수정 전 (fail-open)**:
```cpp
if (pos >= payload.size()) {
    out_db.clear();
    return std::expected<void, ParseError>{};
}
```

**수정 후 (fail-close)**:
```cpp
if (pos >= payload.size()) {
    return std::unexpected(ParseError{
        ParseErrorCode::kMalformedPacket,
        "auth_response length prefix missing",
        std::format("pos={}, payload_size={}", pos, payload.size())
    });
}
```

#### Major 2: CLIENT_CONNECT_WITH_DB db_name 필드 전체 누락 시 fail-open 제거 (line 565-572)

**수정 전 (fail-open)**:
```cpp
if (pos >= payload.size()) {
    // db_name 없음 (null terminator 없이 끝난 경우)
    out_db.clear();
    return std::expected<void, ParseError>{};
}
```

**수정 후 (fail-close)**:
```cpp
if (pos >= payload.size()) {
    return std::unexpected(ParseError{
        ParseErrorCode::kMalformedPacket,
        "database field missing despite CLIENT_CONNECT_WITH_DB flag",
        std::format("pos={}, payload_size={}", pos, payload.size())
    });
}
```

### 2.2 테스트 추가 — tests/test_handshake_auth.cpp

#### FO-1: CLIENT_SECURE_CONNECTION auth_response 길이 prefix 누락 → ParseError

- 테스트명: `ExtractHandshakeResponseFields.SecureConn_AuthLenPrefixMissing_IsError`
- 검증: payload가 username null-terminator 직후에서 잘린 경우 `ParseError(kMalformedPacket)` 반환 및 에러 메시지에 "auth_response length prefix missing" 포함

#### FO-2: CLIENT_CONNECT_WITH_DB 설정 + db_name 필드 전체 누락 → ParseError

- 테스트명: `ExtractHandshakeResponseFields.ConnectWithDb_DbFieldMissing_IsError`
- 검증: auth_response 정상 처리 후 payload 종료 시 `ParseError(kMalformedPacket)` 반환 및 에러 메시지에 "database field missing" 포함

#### 기존 테스트 helper 버그 수정: build_handshake_response cap_flags

- **발견**: `build_handshake_response` 함수의 기본 `cap_flags = 0x00008209U`에 `CLIENT_CONNECT_WITH_DB(0x0008)` 비트가 이미 포함되어 있어, `with_db=false`를 전달해도 플래그가 제거되지 않는 버그 존재.
- **원인**: `0x00008209U = 0x8000(SECURE_CONNECTION) | 0x0200(PROTOCOL_41) | 0x0008(CONNECT_WITH_DB) | 0x0001(LONG_PASSWORD)`
- **영향**: `EX-7(NoConnectWithDb_DbNameIsEmpty)` 테스트가 수정 전에는 fail-open으로 통과했으나, fail-close 수정 후 올바르게 실패를 드러냄.
- **수정**: `0x00008209U` → `0x00008201U` (CLIENT_CONNECT_WITH_DB 비트 제거, with_db 파라미터로만 조건부 설정)

---

## 3. 변경 분류

- `behavior` — 입력 검증 강화 (fail-open → fail-close 전환). 공개 인터페이스 변경 없음.

---

## 4. 인터페이스 영향

- **없음**
- `detail::extract_handshake_response_fields` 함수 시그니처 유지
- `HandshakeRelay::relay_handshake` 공개 인터페이스 변경 없음
- `handshake_detail.hpp` 변경 없음

---

## 5. 운영 영향

| 항목 | 내용 |
|------|------|
| 동작 변화 | auth_response 길이 prefix 누락 패킷 수신 시 세션 종료 (기존: 성공 반환) |
| 동작 변화 | CLIENT_CONNECT_WITH_DB 설정 + db_name 필드 누락 패킷 수신 시 세션 종료 (기존: 성공 반환) |
| 로그 | `ParseError(kMalformedPacket)` 발생 → 기존 에러 로깅 경로로 세션 종료 로그 기록됨 |
| 설정 변경 | 없음 |
| 포트 변경 | 없음 |
| 롤백 포인트 | 이전 커밋으로 revert 시 fail-open 상태로 복구됨 |

---

## 6. 에러/예외 경로 처리 요약

| 경로 | 기존 동작 | 수정 후 동작 |
|------|---------|------------|
| CLIENT_PLUGIN_AUTH_LENENC, pos >= payload.size() | out_db.clear(), 성공 반환 (fail-open) | ParseError(kMalformedPacket, "auth_response length prefix missing") |
| CLIENT_SECURE_CONNECTION, pos >= payload.size() | out_db.clear(), 성공 반환 (fail-open) | ParseError(kMalformedPacket, "auth_response length prefix missing") |
| CLIENT_CONNECT_WITH_DB 설정, db_name 필드 누락 | out_db.clear(), 성공 반환 (fail-open) | ParseError(kMalformedPacket, "database field missing despite CLIENT_CONNECT_WITH_DB flag") |

모든 에러는 `ParseErrorCode::kMalformedPacket`으로 분류되며 `context` 필드에 `pos`, `payload_size` 포함.

---

## 7. 문서 영향 분석

- **변경 동작/인터페이스/운영 영향**: `extract_handshake_response_fields` 내부 검증 강화. 공개 인터페이스/프로토콜 동작 변경 없음.
- **영향 문서 후보**: `docs/interface-reference.md` (extract_handshake_response_fields 에러 조건 설명), `docs/sequences.md` (핸드셰이크 필드 검증 흐름)
- **실제 수정 문서**: 없음 (fail-close 강화는 기존 문서의 "payload 범위 검증" 명시 범위 내 구현 수정)
- **문서 미수정 사유**: 기존 `docs/interface-reference.md`에 `extract_handshake_response_fields`의 실패 조건으로 "payload 범위 검증 강화"가 이미 명시되어 있음. 이번 수정은 그 명시된 계약의 누락 케이스를 올바르게 구현한 것으로, 계약 자체 변경이 아님. Technical-writer에게 추가 문서화 필요 여부 검토 요청.

---

## 8. 테스트 추가/수정 내용

| 번호 | 테스트명 | 분류 | 결과 |
|------|--------|------|------|
| FO-1 | `ExtractHandshakeResponseFields.SecureConn_AuthLenPrefixMissing_IsError` | 신규 | Passed |
| FO-2 | `ExtractHandshakeResponseFields.ConnectWithDb_DbFieldMissing_IsError` | 신규 | Passed |
| EX-7 | `ExtractHandshakeResponseFields.NoConnectWithDb_DbNameIsEmpty` | 기존 (helper 버그 수정) | Passed (수정 후) |

---

## 9. 빌드/테스트 실행 결과

### ASan 빌드

```
cmake --preset asan
cmake --build /workspace/build/asan
```

- 빌드 경고: **없음** (`-Werror` 활성화 상태)
- 컴파일 에러: **없음**

### ASan 테스트

```
cmake --build /workspace/build/asan --target test
```

```
100% tests passed, 0 tests failed out of 98
Total Test time (real) = 2.68 sec
```

- 기존 96개 테스트 전원 통과 (회귀 없음)
- 신규 테스트 2건 (FO-1, FO-2) 통과
- ASan/LeakSanitizer: 클린 (버퍼 오버플로우, use-after-free, 메모리 누수 없음)

---

## 10. 교차영향 및 후속 요청

- **security-engineer**: fail-close 강화로 malformed HandshakeResponse 수신 시 세션 종료 보장됨. 추가 정책 판정 없음.
- **qa-engineer**: 기존 `build_handshake_response` helper에서 `CLIENT_CONNECT_WITH_DB` 비트가 기본값에 포함되어 있어 fail-open 상태에서 잠재적 테스트 오류가 가려져 있었음. 다른 테스트에서 유사한 cap_flags 설정 오류가 있는지 점검 권장.
- **technical-writer**: `docs/interface-reference.md`의 `extract_handshake_response_fields` 에러 조건 설명에 auth_response 길이 prefix 누락 및 db_name 필드 전체 누락 케이스를 명시적으로 추가하는 것을 검토 요청.
- **Architect**: 인터페이스 변경 없음, 승인 불필요.
