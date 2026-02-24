# DON-24 QA 완료 보고서

**작성일**: 2026-02-24
**담당**: qa-engineer
**브랜치**: `feat/DON-24-mysql-protocol`

---

## 1. 변경 파일 목록

- `/workspace/tests/test_mysql_packet.cpp` — 기존 19개 테스트에 11개 신규 테스트 추가 (총 37개)

## 2. 변경 요약

DON-24 이슈 단위 테스트 체크리스트 대비 누락된 케이스를 보강하고, 추가 경계값 케이스를 포함하였다.

### 추가된 테스트 (11개)

| 번호 | 테스트 이름 | 검증 내용 |
|------|-------------|-----------|
| 12 | `ExtractCommand.ComInitDb` | COM_INIT_DB(0x02) → `kComInitDb` 분류, query 필드 비어있음 |
| 13 | `ExtractCommand.ComStmtPrepare` | COM_STMT_PREPARE(0x16) → `kComStmtPrepare` 분류, query 필드 비어있음 |
| 14 | `ExtractCommand.ComQueryMultibyteChars` | COM_QUERY에 UTF-8 멀티바이트(한글) 포함 SQL 바이트 무결성 보존 |
| 15 | `MysqlPacketType.FELargePayloadNotEof` | 0xFE + payload >= 9바이트 → `kEof`가 아닌 `kUnknown` |
| 16 | `MysqlPacketParse.SequenceIdMaxValue` | sequence_id=255(0xFF) 보존 및 직렬화/재파싱 라운드트립 |
| 17 | `MysqlPacketParse.ExactlyFourBytes` | 정확히 4바이트(헤더만, length=0) → 파싱 성공 |
| 18 | `MysqlPacketParse.OneByte` | 1바이트 입력 → `kMalformedPacket` |
| 19 | `MysqlPacketParse.EmptySpan` | 빈 스팬(0바이트) → `kMalformedPacket`, message 비어있지 않음 |
| 20 | `ExtractCommand.ComPing` | COM_PING(0x0E) → `kComPing`, query 비어있음 |
| 21 | `MysqlPacketMakeError.SequenceIdZero` | make_error sequence_id=0 경계값 |
| 22 | `MysqlPacketMakeError.EmptyMessage` | 빈 메시지 ERR 패킷 → payload 최소 9바이트 |

## 3. 변경 분류

- `behavior` (테스트로 행동 명세 강화)

## 4. 인터페이스 영향

- 없음 (헤더/함수 계약 변경 없음, 테스트 파일만 수정)

## 5. 운영 영향

- 없음

## 6. 문서 영향 분석

- **변경 동작/인터페이스/운영 영향**: 테스트 전용 변경. 구현/인터페이스/운영에 영향 없음
- **영향 문서 후보**: 없음
- **실제 수정 문서**: `/workspace/docs/reports/DON-24-qa-report.md` (본 보고서)
- **문서 미수정 사유**: 구현/인터페이스 변경이 없으므로 기존 문서 수정 불필요

## 7. 테스트/검증 결과

### ASan 빌드 결과 (최종)

```
cmake --preset asan && cmake --build build/asan --target dbgate_tests
cd build/asan && ctest --output-on-failure
```

```
100% tests passed, 0 tests failed out of 37
Total Test time (real) = 1.45 sec
```

- 빌드 경고 없음 (`-Werror` 활성화 상태)
- ASan/LeakSanitizer: 클린 (버퍼 오버플로우, use-after-free, 메모리 누수 없음)

### TSan 빌드 결과

```
cmake --preset tsan && cmake --build build/tsan --target dbgate_tests
```

- **TSan 컴파일**: 성공 (바이너리 생성 완료)
- **TSan 실행**: 실패

```
FATAL: ThreadSanitizer: unexpected memory mapping
```

- **원인 분석**: 환경 제약 (아키텍처/커널 이슈). aarch64(arm64) Linux에서 `mmap_rnd_bits=33`이 설정되어 있으나, TSan은 aarch64에서 `mmap_rnd_bits <= 28`을 요구함. 코드 결함이 아닌 CI 환경의 ASLR 설정 문제임.
- **해결 방안 (후속 요청)**: infra-engineer에게 CI 환경의 `mmap_rnd_bits` 조정 또는 x86_64 TSan 실행 환경 추가 요청 필요.

### DON-24 이슈 단위 테스트 체크리스트 대비

| 케이스 | 기존 | 보강 후 |
|--------|------|---------|
| 1. 정상 패킷 파싱 (다양한 길이) | O | O |
| 2. COM_QUERY SQL 추출 | O | O |
| 3. ERR 패킷 파싱 | O | O |
| 4. OK 패킷 파싱 | O | O |
| 5. 4바이트 미만 → ParseError | O | O |
| 6. length > 실제 데이터 → ParseError | O | O |
| 7. 핸드셰이크 패킷 타입 판별 | O | O |
| 8. 에러 패킷 빌드 → 올바른 포맷 | O | O |
| 9. COM_QUIT, COM_INIT_DB 분류 | COM_QUIT만 | O (COM_INIT_DB 추가) |
| 10. COM_STMT_PREPARE 탐지 | X | O (신규 추가) |

### 경계값 체크리스트

| 경계값 | 검증 여부 |
|--------|-----------|
| 빈 패킷(0바이트) | O (`EmptySpan`) |
| 1바이트 패킷 | O (`OneByte`) |
| 4바이트 정확히(헤더만) | O (`ExactlyFourBytes`) |
| sequence_id 최대값(255) | O (`SequenceIdMaxValue`) |
| 0xFE payload < 9 → kEof | O (기존 `EofPacket`) |
| 0xFE payload >= 9 → kUnknown | O (`FELargePayloadNotEof`) |
| UTF-8 멀티바이트 문자 | O (`ComQueryMultibyteChars`) |
| make_error 빈 메시지 | O (`EmptyMessage`) |
| make_error seq_id=0 | O (`SequenceIdZero`) |

## 8. 발견된 이슈

### 구현 결함: 없음

모든 테스트가 ASan 하에서 예상 동작을 검증함. 구현 버그 없음.

### 환경 제약 발견

- **TSan 실행 불가** (aarch64 환경 `mmap_rnd_bits=33` 문제)
  - 이 문제는 코드 결함이 아님
  - 재현 명령: `cmake --preset tsan && cmake --build build/tsan && build/tsan/dbgate_tests`
  - 환경 정보: `uname -m` → aarch64, `cat /proc/sys/vm/mmap_rnd_bits` → 33
  - 수정 방안: `sysctl -w vm.mmap_rnd_bits=28` 또는 x86_64 환경에서 실행

### 코드 품질 관찰 (테스트성 측면)

- `u8""` 리터럴이 C++20+에서 `char8_t` 타입을 반환하므로 `std::string`에 직접 대입 불가.
  본 테스트에서는 명시적 `\xNN` 이스케이프로 대체함. (테스트 파일 내 한정된 이슈, 구현 코드 무관)

## 9. 교차영향 및 후속 요청

- **infra-engineer**: CI 환경에서 TSan 실행 가능 여부 확인 필요. aarch64에서 TSan을 실행하려면 `vm.mmap_rnd_bits <= 28` 설정 또는 x86_64 runner 추가를 검토해야 함.
- **Architect**: TSan 환경 제약을 CI 파이프라인 문서에 반영 여부 결정 필요.
- 현재 기준 구현 버그 없음, 테스트 커버리지 DON-24 체크리스트 100% 달성.
