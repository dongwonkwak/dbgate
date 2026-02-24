# QA Engineer Persistent Memory

## Project: dbgate (DB Access Control Proxy)

### Key Architecture Facts
- C++23, GCC 14, Boost.Asio co_await, std::expected<T,E>
- Build presets: default/debug/asan/tsan in build/{preset}/
- Tests binary: build/{preset}/dbgate_tests
- CMakeLists.txt: dbgate_tests target lists test sources explicitly (must update for new files)

### Environment Constraints
- Platform: aarch64 (arm64) Linux, kernel 6.8.0
- TSan DOES NOT RUN on this machine: `mmap_rnd_bits=33`, TSan requires <=28 on aarch64
  - TSan binary compiles successfully but crashes at startup with "unexpected memory mapping"
  - This is NOT a code defect — environment limitation only
  - Fix: `sysctl -w vm.mmap_rnd_bits=28` (requires root) or use x86_64 runner
- ASan works fine on this machine

### C++20/23 Pitfalls in Tests
- `u8"..."` string literals produce `char8_t` (NOT `char`) in C++20+
  - Cannot assign to `std::string` directly → use `"\xNN..."` hex escapes instead
  - Compiler error: "conversion from const char8_t[N] to non-scalar type std::string"

### Test File Locations
- `/workspace/tests/test_mysql_packet.cpp` — 37 tests (DON-24 complete)
- `/workspace/tests/test_logger.cpp` — structured logger tests

### DON-24 Status (as of 2026-02-24)
- All 37 tests pass under ASan
- DON-24 checklist: 10/10 cases covered
- See `/workspace/docs/reports/DON-24-qa-report.md` for details

### Key Protocol Notes
- 0xFE payload: kEof only if payload.size() < 9, else kUnknown
- COM_QUERY (0x03): only command type that populates `query` field in CommandPacket
- sequence_id max = 255 (uint8_t), preserved across serialize/parse roundtrip
- make_error payload minimum = 9 bytes: 0xFF + 2-byte code + '#' + 5-byte sql_state

### Test Patterns
- Wire format helper: push 3-byte LE length + seq_id byte + payload bytes
- Always check both `.has_value()` and `.error().code` for error paths
- Roundtrip tests: parse → serialize → re-parse to verify identity
