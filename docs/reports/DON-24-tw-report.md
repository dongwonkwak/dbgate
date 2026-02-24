# DON-24 문서 업데이트 보고서 (8-state 리팩터링 반영)

**작성일**: 2026-02-24
**담당**: technical-writer
**브랜치**: `feat/DON-24-mysql-protocol`

---

## 1. 작업 개요

network-engineer가 구현한 **MySQL 핸드셰이크 8-state 상태 머신 리팩터링**을 반영하여 문서를 업데이트했습니다.

### 변경 범위
- 공개 헤더(`.hpp`) 변경: **없음**
- 내부 구현 (`handshake.cpp`): 상태 머신 리팩터링
- 신규 테스트 전용 헤더: `src/protocol/handshake_detail.hpp` (내부 인터페이스)

### 반영 대상 문서
- `docs/sequences.md` — 시나리오 1 (MySQL 핸드셰이크 패스스루)
- `docs/interface-reference.md` — protocol/handshake.hpp 섹션

---

## 2. 변경 파일 목록

| 파일 | 변경 유형 | 변경 사유 |
|------|---------|---------|
| `/workspace/docs/sequences.md` | 보강 | 4단계 → 8-state 상태 머신 명시화, 라운드트립 제한/Unknown 패킷 처리 추가 |
| `/workspace/docs/interface-reference.md` | 보강 | 8-state 상태 전이 테이블 추가, kTerminateNoRelay/라운드트립 제한 명시 |

---

## 3. 변경 요약

### 3.1 docs/sequences.md — 시나리오 1 업데이트

#### 변경 전 (4단계 모델)
```
1. Initial Handshake 수신
2. Handshake Response 수신
3. Server Auth Response 수신 (OK/ERR/AuthSwitch 분기)
4. Completion: SessionContext 업데이트
```

#### 변경 후 (8-state 상태 머신)
```
상태:
- kWaitServerGreeting → kWaitClientResponse → kWaitServerAuth
- kWaitServerAuth → {kDone | kWaitClientAuthSwitch | kWaitClientMoreData | kFailed}
- kWaitClientAuthSwitch → kWaitServerAuthSwitch
- kWaitServerAuthSwitch → {kDone | kWaitClientMoreData | kFailed}
- kWaitClientMoreData → kWaitServerMoreData
- kWaitServerMoreData → {kDone | kWaitClientMoreData | kFailed}

액션: kRelayToClient, kRelayToServer, kComplete, kTerminate, kTerminateNoRelay

주요 신기능:
- AuthSwitch 중 AuthSwitch (중첩) → ParseError (fail-close)
- AuthMoreData 중 AuthSwitch (순서 위반) → ParseError (fail-close)
- Unknown 패킷 → kTerminateNoRelay (ERR 전달 없이 즉시 종료)
- 라운드트립 카운터: 최대 10회 (kMaxRoundTrips=10)
- 라운드트립 초과 → ParseError (fail-close)
```

#### 추가 내용
- 8개 상태 정의 및 전이 다이어그램 (Mermaid)
- 6가지 AuthResponseType 분류 (kOk, kError, kEof, kAuthSwitch, kAuthMoreData, kUnknown)
- 상태별 액션 명시
- 라운드트립 제한 로직
- extract_handshake_response_fields 강화 내용:
  - 최소 길이 검증 (payload.size() < 33)
  - length-encoded 인증 응답 (0xFC/0xFD) 처리
  - 0xFE/0xFF 사용 시 비정상 판정
  - null terminator 검증 추가

### 3.2 docs/interface-reference.md — handshake.hpp 섹션 업데이트

#### 변경 전 (간단한 4단계)
```
1. Initial Handshake
2. Handshake Response (username/database 추출)
3. Server Auth Response (0xFF/0xFE/0x00 분기)
4. Completion
```

#### 변경 후 (8-state 상세 설명)
- **상태별 상세 설명**: kWaitServerGreeting → ... → kDone/kFailed
- **상태 전이 테이블**: 응답 타입별 다음 상태/액션 명시
- **AuthSwitch 라운드트립**: kWaitClientAuthSwitch → kWaitServerAuthSwitch
- **AuthMoreData 라운드트립 체인**: kWaitClientMoreData ↔ kWaitServerMoreData
- **라운드트립 제한**: round_trips >= kMaxRoundTrips → ParseError
- **Unknown 패킷**: kTerminateNoRelay (ERR 전달 없음)
- **강화된 필드 추출**:
  - length-encoded auth_response: 1/2/3바이트 길이 인코딩
  - CLIENT_PLUGIN_AUTH_LENENC: 0xFE/0xFF 비정상 처리
  - payload 범위 검증 강화
  - null terminator 검증 (username, database)

#### 추가 내용
- AuthResponseType 정의 및 분류 기준
- 상태 전이 의사 결정 테이블
- fail-close 보장 조건 상세화
- 에러 처리 경로 명시

---

## 4. 변경 분류

### 분류: `behavior-doc`, `interface-doc`

- **behavior-doc**: 8-state 상태 머신 동작, 라운드트립 제한, Unknown 패킷 처리 등 **동작 상세화**
- **interface-doc**: 상태/액션/응답 타입 정의 및 상태 전이 테이블 추가 → **인터페이스 명확화**

---

## 5. 인터페이스/동작 문서화 영향

### 인터페이스 변경: **없음**
- `HandshakeRelay::relay_handshake` 함수 시그니처: 변경 없음
- `protocol/handshake.hpp` 공개 인터페이스: 변경 없음
- SessionContext 구조: 변경 없음
- ParseError 구조: 변경 없음

### 동작 변경: **있음** (내부 상태 머신 구조)
- 이전: 내부 상태 관리 방식 미명시 (black box)
- 현재: 8-state 상태 머신으로 명시화
- **공개 계약에 영향 없음**: 입력/출력 동작은 동일 (투명 릴레이 + 최소 파싱)
- **내부 동작 명확화**: 상태 전이, 에러 처리, 라운드트립 제한 등

---

## 6. 운영 문서 영향

### 운영 영향: **있음** (문서화)

#### 새로 명시된 운영 고려사항
| 항목 | 내용 |
|------|------|
| **라운드트립 제한** | 최대 10회 (kMaxRoundTrips=10), 초과 시 세션 종료 |
| **Unknown 패킷** | 0x00/0xFF/0xFE/0x01 이외 패킷 수신 시 ERR 전달 없이 즉시 종료 (fail-close) |
| **AuthSwitch 중첩** | 감지 시 ParseError 발생 (fail-close) |
| **필드 검증 강화** | Handshake Response 필드 (username, database) null terminator/payload 범위 검증 |
| **로그 기록 대상** | 라운드트립 제한 초과, Unknown 패킷, AuthSwitch 중첩 등 fail-close 경로 |

#### 기존 동작
- 패킷 읽기/쓰기 실패, 서버 ERR 패킷 등 (변경 없음)

---

## 7. 문서 영향 분석

### 변경 동작/인터페이스/운영 영향
- **동작**: 8-state 상태 머신으로 핸드셰이크 진행 상세 명시
  - 기존: 일반적 4단계 (추상)
  - 현재: 9개 상태(kWaitServerGreeting ~ kFailed)와 명확한 전이 규칙
- **인터페이스**: 상태/액션/응답 타입 정의 공개 (테스트/모니터링 용도)
  - 공개 헤더 변경 없음, 문서 명시만 추가
- **운영**: 라운드트립 제한/Unknown 패킷/AuthSwitch 중첩 처리 명시
  - 모니터링: 이들 fail-close 경로 로그 감시 권장

### 영향 문서 후보
- `docs/sequences.md` — ✓ 수정 완료
- `docs/interface-reference.md` — ✓ 수정 완료
- `docs/architecture.md` — 영향 없음 (일반적 아키텍처, 상태 머신 상세는 불필요)
- `docs/adr/002-handshake-passthrough.md` — 영향 없음 (패스스루 원칙 유지)
- `docs/observability.md` — 미존재 (Phase 3~4에서 작성)
- `docs/failure-modes.md` — 미존재, 후속: 핸드셰이크 실패 모드 기록 권장

### 실제 수정 문서
1. `/workspace/docs/sequences.md` — 시나리오 1 전체 재작성
2. `/workspace/docs/interface-reference.md` — handshake.hpp 섹션 확대

### 문서 미수정 사유
- **docs/architecture.md**: 일반적 계층 다이어그램, 상태 머신 상세는 interface-reference.md 담당
- **docs/adr/002-handshake-passthrough.md**: 설계 결정(패스스루) 변경 없음
- **docs/observability.md**: 미존재 (로그/모니터링은 Phase 3 이후 작성)

---

## 8. 코드/설정과 교차검증한 항목

### 검증 대상 파일

| 파일 | 라인 | 검증 내용 |
|------|------|---------|
| `src/protocol/handshake_detail.hpp` | 전체 | HandshakeState, HandshakeAction, AuthResponseType enum 정의 확인 |
| `src/protocol/handshake.cpp` | 32-110 | read_packet, write_packet 헬퍼 함수 (구현 일치) |
| `src/protocol/handshake.cpp` | 128-159 | classify_auth_response (6가지 타입 분류) |
| `src/protocol/handshake.cpp` | 162-398 | process_handshake_packet (순수 함수, 상태 전이 로직) |
| `src/protocol/handshake.cpp` | 400-591 | extract_handshake_response_fields (강화 버전) |
| `src/protocol/handshake.cpp` | 602-732 | HandshakeRelay::relay_handshake (I/O 루프) |

### 검증 결과

#### ✓ HandshakeState 정의 확인
- 8개 상태: kWaitServerGreeting, kWaitClientResponse, kWaitServerAuth, kWaitClientAuthSwitch, kWaitServerAuthSwitch, kWaitClientMoreData, kWaitServerMoreData, kDone, kFailed
- 문서 기술 완전 일치

#### ✓ HandshakeAction 정의 확인
- 5가지 액션: kRelayToClient, kRelayToServer, kComplete, kTerminate, kTerminateNoRelay
- 문서 기술 완전 일치

#### ✓ AuthResponseType 분류 확인
- 6가지 타입: kOk, kError, kEof, kAuthSwitch, kAuthMoreData, kUnknown
- classify_auth_response 구현 (라인 133-159) 일치:
  - 0x00 → kOk
  - 0xFF → kError
  - 0xFE + payload < 9 → kEof
  - 0xFE + payload >= 9 → kAuthSwitch
  - 0x01 → kAuthMoreData
  - 기타 → kUnknown

#### ✓ 상태 전이 로직 확인
- `process_handshake_packet` (라인 162-398):
  - kWaitServerGreeting → kWaitClientResponse (무조건 kRelayToClient) ✓
  - kWaitClientResponse → kWaitServerAuth (무조건 kRelayToServer) ✓
  - kWaitServerAuth → 6가지 분기 (OK/ERR/EOF/AuthSwitch/AuthMoreData/Unknown) ✓
  - kWaitClientAuthSwitch → kWaitServerAuthSwitch (무조건 kRelayToServer) ✓
  - kWaitServerAuthSwitch → 5가지 분기 (OK/ERR/EOF/AuthMoreData/AuthSwitch 중첩 에러/Unknown) ✓
  - kWaitClientMoreData → kWaitServerMoreData (무조건 kRelayToServer) ✓
  - kWaitServerMoreData → 5가지 분기 (OK/ERR/EOF/AuthMoreData/AuthSwitch 에러/Unknown) ✓
  - 종단 상태 (kDone/kFailed) → 추가 처리 금지 ✓

#### ✓ 라운드트립 제한 확인
- kMaxRoundTrips = 10 (상수, 라인 33) ✓
- round_trips >= kMaxRoundTrips 체크 (라인 283, 349):
  - kWaitServerAuthSwitch에서 AuthMoreData 수신 시
  - kWaitServerMoreData에서 AuthMoreData 수신 시
  - ParseError 반환 ✓

#### ✓ extract_handshake_response_fields 강화 확인
- 최소 길이 검증: payload.size() < 33 → ParseError (라인 422-427) ✓
- Capability flags 읽기: offset 0-3 (라인 431-435) ✓
- Username 추출: offset 32, null-terminated (라인 448-465) ✓
- Auth response length-encoded (CLIENT_PLUGIN_AUTH_LENENC, 0x00200000):
  - 0xFC: 2바이트 (라인 487-498) ✓
  - 0xFD: 3바이트 (라인 499-511) ✓
  - 0xFE/0xFF: 비정상 → ParseError (라인 513-518) ✓
- Auth response 1바이트 길이 (CLIENT_SECURE_CONNECTION, 0x00008000):
  - (라인 531-548) ✓
- Auth response null-terminated (없음):
  - (라인 550-558) ✓
- 모든 경우 payload 범위 검증 ✓
- Database 추출: CLIENT_CONNECT_WITH_DB 확인 (라인 561-588) ✓
- Database null terminator 검증 (라인 574-579) ✓

#### ✓ relay_handshake I/O 루프 확인
- 상태별 소켓 선택 (라인 626-632) ✓
- 패킷 읽기 (라인 637-640) ✓
- Handshake Response에서 username/db_name 추출 (1회만, 라인 646-654) ✓
- process_handshake_packet 호출 (라인 657-660) ✓
- 액션 수행 (라인 665-713):
  - kRelayToClient/kRelayToServer (라인 666-678) ✓
  - kComplete (라인 680-690): ctx 업데이트 + 반환 ✓
  - kTerminate (라인 692-701): ERR 패킷 전달 후 ParseError 반환 ✓
  - kTerminateNoRelay (라인 703-711): ERR 전달 없이 ParseError 반환 ✓
- 라운드트립 카운터 증가 (라인 716-720) ✓
- 상태 전이 (라인 723) ✓

### 검증 결론
✓ **코드-문서 완전 일치**: 8-state 구현과 문서 명시 완벽하게 정렬

---

## 9. 남은 빈칸/추가 확인 필요 항목

### Phase 3~4 진행 시 추가 작업
1. **handshake 통합 테스트 (클라이언트-프록시-서버 end-to-end)**
   - 단위 테스트(handshake_detail.hpp)와 통합 테스트 작성 필요
   - QA 에이전트 담당

2. **관찰성(observability) 문서 작성**
   - 라운드트립 제한/Unknown 패킷/AuthSwitch 중첩 등 fail-close 경로 로깅
   - 모니터링 메트릭, 로그 레벨, 샘플 로그 포함
   - infra-engineer/TW 협력

3. **장애 모드 문서 (failure-modes.md)**
   - 핸드셰이크 실패 시나리오 (라운드트립 초과, 필드 검증 실패 등)
   - TW 담당

4. **threat model 업데이트 (Phase 4)**
   - 핸드셰이크 우회/조작 공격 시나리오 추가
   - TW 담당

---

## 10. 교차영향 및 후속 요청

### 즉시 확인 필요
- **없음**: 문서화만 수행, 인터페이스/동작/배포 영향 없음

### Phase 3 시점 요청

| 대상 | 항목 | 사유 |
|------|------|------|
| QA engineer | handshake 통합 테스트 케이스 | 8-state 상태 전이 커버리지, 라운드트립 제한 테스트 |
| Infra engineer | observability 문서/로깅 | 라운드트립/Unknown 패킷 모니터링 |
| Architect | 성능 프로파일링 | 라운드트립 10회 제한이 실제 워크로드에 영향 없는지 확인 |

### Phase 4+ 요청
| 대상 | 항목 | 사유 |
|------|------|------|
| TW | threat-model.md | Handshake 공격 시나리오 (패킷 변조, 상태 혼동, 라운드트립 폭탄) |
| TW | failure-modes.md | 핸드셰이크 장애 모드 정리 |
| QA | 부하 테스트 | 라운드트립 10회가 정상 플러그인에서 발생하는지, DoS 가능성 |

---

## 11. 최종 보고

### 11.1 작업 완료 상태
✓ **완료** — 8-state 상태 머신 문서화

### 11.2 변경 통계
| 항목 | 수량 |
|------|------|
| 수정 문서 | 2 |
| 추가 섹션 | 상태 머신 상세(sequences.md), 상태 전이 테이블(interface-reference.md) |
| 코드 일치 검증 | 6개 구현 영역, 모두 완전 일치 |
| 문서-코드 불일치 | 0건 |

### 11.3 검증 프로세스
1. ✓ handshake_detail.hpp 8개 enum 정의 확인
2. ✓ handshake.cpp classify_auth_response (6가지 분류) 일치
3. ✓ handshake.cpp process_handshake_packet (상태 전이 9가지) 일치
4. ✓ handshake.cpp extract_handshake_response_fields 강화 사항 일치
5. ✓ handshake.cpp relay_handshake I/O 루프 일치
6. ✓ 라운드트립 제한(kMaxRoundTrips=10) 구현/문서 일치

### 11.4 인수인계 체크리스트
- ✓ 8-state 상태 정의 명시
- ✓ 상태 전이 규칙 테이블화
- ✓ 6가지 응답 타입 분류
- ✓ 5가지 액션(relay/complete/terminate/terminate-no-relay) 명시
- ✓ 라운드트립 제한(10회) 명시
- ✓ Unknown 패킷 처리(fail-close) 명시
- ✓ AuthSwitch 중첩 금지 명시
- ✓ 강화된 필드 검증 명시
- ✓ 에러 경로 완전 포함
- ✓ 코드 예시로 구현 세부사항 명확화

---

## 12. 참고 자료

### 구현 파일
- `/workspace/src/protocol/handshake_detail.hpp` (신규, 테스트 전용 헤더)
- `/workspace/src/protocol/handshake.cpp` (리팩터링된 구현)
- `/workspace/src/protocol/mysql_packet.cpp`
- `/workspace/src/protocol/command.cpp`

### 관련 문서
- `/workspace/docs/sequences.md` (시나리오 1 — ✓ 수정 완료)
- `/workspace/docs/interface-reference.md` (protocol/handshake.hpp — ✓ 수정 완료)
- `/workspace/docs/architecture.md` (일반 아키텍처 — 변경 없음)
- `/workspace/docs/adr/002-handshake-passthrough.md` (설계 결정 — 변경 없음)
- `/workspace/docs/project-spec-v3.md` (프로젝트 사양 — 변경 없음)

### 테스트
- `/workspace/tests/test_handshake_auth.cpp` (신규 테스트, QA 담당)
- `/workspace/tests/test_mysql_packet.cpp` (기존, MysqlPacket 관련)

---

## 부록: 상태 머신 다이어그램 참조

### 8-state 전체 구조
```
시작: kWaitServerGreeting
   ↓ (read server greeting)
kWaitClientResponse
   ↓ (extract username/db_name, relay to server)
kWaitServerAuth (classify_auth_response)
   ├→ OK (0x00) → kDone (complete)
   ├→ Error (0xFF) → kFailed (terminate)
   ├→ EOF (0xFE, <9) → kFailed (terminate)
   ├→ AuthSwitch (0xFE, >=9) → kWaitClientAuthSwitch → kWaitServerAuthSwitch
   │                               ├→ OK → kDone (complete)
   │                               ├→ AuthMoreData → kWaitClientMoreData (round_trips++)
   │                               ├→ Error → kFailed (terminate)
   │                               └→ AuthSwitch 중첩 → ParseError (fail-close)
   ├→ AuthMoreData (0x01) → kWaitClientMoreData → kWaitServerMoreData
   │                            ├→ OK → kDone (complete)
   │                            ├→ AuthMoreData → kWaitClientMoreData (round_trips++)
   │                            ├→ Error → kFailed (terminate)
   │                            └→ AuthSwitch → ParseError (fail-close)
   └→ Unknown (0x??) → kFailed (terminateNoRelay)

라운드트립 제한: kMaxRoundTrips = 10
  - round_trips >= 10 → ParseError (fail-close)
```

