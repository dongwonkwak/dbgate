// ---------------------------------------------------------------------------
// test_injection_detector.cpp
//
// InjectionDetector 단위 테스트.
//
// [테스트 범위]
// - 기본 10가지 인젝션 패턴 탐지 성공
// - 정상 쿼리 false positive 없음 확인
// - 빈 패턴 목록 동작 확인
// - 대소문자 무관 탐지 (icase)
// - 잘못된 정규식 패턴 처리 (건너뜀, 나머지 유효 패턴 적용)
//
// [오탐/미탐 트레이드오프]
// - UNION SELECT 패턴: 합법적인 UNION ALL 페이징에서 false positive 가능.
// - OR 패턴: 특정 형태만 탐지하므로 다른 형태(OR true 등)는 미탐.
// - 주석 분할 우회(UN/**/ION SELECT)는 탐지하지 못함 (알려진 미탐).
// - 인코딩 우회(URL, hex)는 탐지하지 못함 (알려진 미탐).
// ---------------------------------------------------------------------------

#include "parser/injection_detector.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 기본 탐지 패턴 (config에서 로드될 기본 패턴)
//
// [패턴 8 — piggyback 변경 이력]
// - DON-25 핫픽스: CALL, PREPARE, EXECUTE, TRUNCATE 를 piggyback 목록에 추가.
//   이유: 세미콜론 이후 CALL/PREPARE/EXECUTE 로 숨겨진 동적 SQL 우회,
//         TRUNCATE 로 테이블 전체 삭제 시도를 인젝션 탐지 레이어에서도 감지.
//
// [오탐/미탐 트레이드오프 — 패턴 8 확장]
// - 오탐(false positive): 합법적인 다중 구문 배치에서 CALL/TRUNCATE 를
//   포함하는 경우 차단될 수 있다. 그러나 멀티 스테이트먼트 자체가
//   sql_parser 레이어에서 이미 fail-close 처리되므로 이중 방어 효과.
// - 미탐(false negative): 세미콜론 없이 인라인으로 결합된 PREPARE/EXECUTE
//   우회는 별도 procedure_detector 와 조합하여 탐지해야 한다.
// ---------------------------------------------------------------------------
static std::vector<std::string> default_patterns() {
    return {
        // 1. UNION 기반 인젝션
        "UNION\\s+SELECT",
        // 2. tautology (OR 기반)
        "'\\s*OR\\s+['\"\\d]",
        // 3. time-based blind: SLEEP
        "SLEEP\\s*\\(",
        // 4. time-based blind: BENCHMARK
        "BENCHMARK\\s*\\(",
        // 5. 파일 읽기
        "LOAD_FILE\\s*\\(",
        // 6. 파일 쓰기
        "INTO\\s+OUTFILE",
        // 7. 파일 덤프
        "INTO\\s+DUMPFILE",
        // 8. piggyback 공격 (CALL/PREPARE/EXECUTE/TRUNCATE 포함)
        //    [DON-25] CALL, PREPARE, EXECUTE, TRUNCATE 추가
        ";\\s*(DROP|DELETE|UPDATE|INSERT|ALTER|CREATE|CALL|PREPARE|EXECUTE|TRUNCATE)",
        // 9. 주석 꼬리 무력화
        "--\\s*$",
        // 10. 인라인 주석 우회
        "/\\*.*\\*/",
    };
}

// ---------------------------------------------------------------------------
// 기본 패턴 10가지 — 탐지 성공 테스트
// ---------------------------------------------------------------------------

TEST(InjectionDetector, UnionSelect) {
    // [오탐 가능성] 합법적인 UNION ALL 페이징 쿼리에서도 탐지될 수 있음
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT * FROM t UNION SELECT 1,2,3");
    EXPECT_TRUE(result.detected)
        << "UNION SELECT should be detected as injection";
    EXPECT_FALSE(result.matched_pattern.empty());
    EXPECT_FALSE(result.reason.empty());
}

TEST(InjectionDetector, Tautology) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT * FROM t WHERE id='1' OR '1'='1'");
    EXPECT_TRUE(result.detected)
        << "OR tautology should be detected";
}

TEST(InjectionDetector, SleepCall) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT SLEEP(5)");
    EXPECT_TRUE(result.detected)
        << "SLEEP() should be detected as time-based injection";
}

TEST(InjectionDetector, BenchmarkCall) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT BENCHMARK(1000000, SHA1('test'))");
    EXPECT_TRUE(result.detected)
        << "BENCHMARK() should be detected as time-based injection";
}

TEST(InjectionDetector, LoadFile) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT LOAD_FILE('/etc/passwd')");
    EXPECT_TRUE(result.detected)
        << "LOAD_FILE() should be detected";
}

TEST(InjectionDetector, IntoOutfile) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT * FROM t INTO OUTFILE '/tmp/out'");
    EXPECT_TRUE(result.detected)
        << "INTO OUTFILE should be detected";
}

TEST(InjectionDetector, IntoDumpfile) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT * FROM t INTO DUMPFILE '/tmp/d'");
    EXPECT_TRUE(result.detected)
        << "INTO DUMPFILE should be detected";
}

TEST(InjectionDetector, Piggyback) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT 1; DROP TABLE users");
    EXPECT_TRUE(result.detected)
        << "Piggyback DROP after semicolon should be detected";
}

TEST(InjectionDetector, PiggybackDelete) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT 1; DELETE FROM users");
    EXPECT_TRUE(result.detected)
        << "Piggyback DELETE after semicolon should be detected";
}

// [DON-25] piggyback 패턴 확장 — CALL, PREPARE, EXECUTE, TRUNCATE
TEST(InjectionDetector, PiggybackCall) {
    // CALL 로 관리자 프로시저를 숨기는 piggyback 공격
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT 1; CALL admin_proc()");
    EXPECT_TRUE(result.detected)
        << "Piggyback CALL after semicolon should be detected";
}

TEST(InjectionDetector, PiggybackPrepare) {
    // PREPARE 로 동적 SQL 을 숨기는 piggyback 공격
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT 1; PREPARE stmt FROM 'SELECT 1'");
    EXPECT_TRUE(result.detected)
        << "Piggyback PREPARE after semicolon should be detected";
}

TEST(InjectionDetector, PiggybackExecute) {
    // EXECUTE 로 사전 준비된 동적 SQL 을 실행하는 piggyback 공격
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT 1; EXECUTE stmt");
    EXPECT_TRUE(result.detected)
        << "Piggyback EXECUTE after semicolon should be detected";
}

TEST(InjectionDetector, PiggybackTruncate) {
    // TRUNCATE 로 테이블 전체 삭제를 시도하는 piggyback 공격
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT 1; TRUNCATE users");
    EXPECT_TRUE(result.detected)
        << "Piggyback TRUNCATE after semicolon should be detected";
}

TEST(InjectionDetector, TrailingComment) {
    InjectionDetector detector(default_patterns());
    // -- 주석 꼬리: 뒤의 조건을 무력화하는 인젝션 기법
    const auto result = detector.check("SELECT * FROM t WHERE id=1 --");
    EXPECT_TRUE(result.detected)
        << "Trailing -- comment should be detected";
}

TEST(InjectionDetector, InlineComment) {
    InjectionDetector detector(default_patterns());
    // 인라인 주석으로 키워드 분할 우회 시도
    const auto result = detector.check("DROP/**/TABLE users");
    EXPECT_TRUE(result.detected)
        << "Inline comment /* */ should be detected";
}

// ---------------------------------------------------------------------------
// 정상 쿼리 — false positive 없음 확인
//
// [오탐 주의]
// 아래 쿼리들은 합법적인 패턴이므로 탐지되어서는 안 됨.
// 만약 이 테스트가 실패하면 패턴 튜닝이 필요함 (false positive 과다).
// ---------------------------------------------------------------------------

TEST(InjectionDetector, NormalSelect) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT id, name FROM users WHERE id=1");
    EXPECT_FALSE(result.detected)
        << "Normal SELECT should NOT be flagged as injection";
}

TEST(InjectionDetector, NormalInsert) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("INSERT INTO logs(msg) VALUES('test')");
    EXPECT_FALSE(result.detected)
        << "Normal INSERT should NOT be flagged as injection";
}

TEST(InjectionDetector, NormalUpdate) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("UPDATE config SET val='new' WHERE key='k'");
    EXPECT_FALSE(result.detected)
        << "Normal UPDATE should NOT be flagged as injection";
}

TEST(InjectionDetector, NormalJoin) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check(
        "SELECT u.id, o.total FROM users u JOIN orders o ON u.id = o.user_id WHERE u.active = 1");
    EXPECT_FALSE(result.detected)
        << "Normal JOIN query should NOT be flagged";
}

TEST(InjectionDetector, NormalDeleteWithWhere) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("DELETE FROM sessions WHERE expired_at < NOW()");
    EXPECT_FALSE(result.detected)
        << "Normal DELETE with WHERE should NOT be flagged";
}

// ---------------------------------------------------------------------------
// 빈 패턴 목록 테스트 — Fail-close 동작
//
// [DON-25 핫픽스] 빈 패턴 목록 또는 전체 무효 패턴 목록은 fail-close 동작.
// check() 호출 시 항상 detected=true, reason="no valid patterns loaded" 반환.
//
// [변경 이유]
// 이전 동작: 빈 패턴 목록 → detected=false (모든 SQL 통과 = fail-open).
// 새 동작:   빈 패턴 목록 → detected=true  (모든 SQL 차단 = fail-close).
//
// [트레이드오프]
// - false positive 증가: 패턴 설정 오류 시 정상 쿼리도 차단됨.
// - false negative 감소: 패턴 없이 인젝션이 통과하는 보안 구멍을 제거.
// - 보안 우선 원칙에 따라 fail-close 를 선택한다.
// ---------------------------------------------------------------------------

TEST(InjectionDetector, EmptyPatterns) {
    // [DON-25] fail-close: 빈 패턴 목록 → 항상 detected=true
    InjectionDetector detector({});
    const auto result = detector.check("SELECT * FROM t UNION SELECT 1,2,3");
    EXPECT_TRUE(result.detected)
        << "With empty pattern list, fail-close should block all SQL (detected=true)";
    EXPECT_EQ(result.reason, "no valid patterns loaded")
        << "reason should indicate no patterns were loaded";
}

// ---------------------------------------------------------------------------
// 대소문자 무관 탐지 테스트 (icase 플래그)
// ---------------------------------------------------------------------------

TEST(InjectionDetector, CaseInsensitive) {
    InjectionDetector detector(default_patterns());
    // 소문자 "union select" → 탐지되어야 함
    const auto result = detector.check("union select 1");
    EXPECT_TRUE(result.detected)
        << "Lowercase 'union select' should be detected (case-insensitive)";
}

TEST(InjectionDetector, CaseInsensitiveSleep) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT sleep(5)");
    EXPECT_TRUE(result.detected)
        << "Lowercase 'sleep()' should be detected";
}

TEST(InjectionDetector, CaseInsensitiveMixed) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SeLeCt * FrOm t UnIoN SeLeCt 1");
    EXPECT_TRUE(result.detected)
        << "Mixed case UNION SELECT should be detected";
}

// ---------------------------------------------------------------------------
// 잘못된 정규식 패턴 처리 테스트
//
// 잘못된 패턴은 건너뛰고 나머지 유효한 패턴은 계속 적용.
// [보안 트레이드오프] 잘못된 패턴 건너뜀 → 탐지 범위 일부 감소 (false negative 증가).
// fail-open 방지: 유효한 패턴들은 그대로 동작.
// ---------------------------------------------------------------------------

TEST(InjectionDetector, InvalidPatternSkipped) {
    // 잘못된 패턴 "[invalid" + 유효한 패턴 "UNION\\s+SELECT"
    std::vector<std::string> patterns = {
        "[invalid_regex",        // 잘못된 패턴: 건너뜀
        "UNION\\s+SELECT",       // 유효한 패턴: 적용됨
    };
    // 생성자에서 로그 경고 후 잘못된 패턴 건너뜀 (예외 미발생)
    InjectionDetector detector(std::move(patterns));

    // 유효한 패턴으로 탐지 동작
    const auto result = detector.check("SELECT 1 UNION SELECT 2");
    EXPECT_TRUE(result.detected)
        << "Valid pattern should still work after invalid pattern is skipped";
}

TEST(InjectionDetector, AllInvalidPatterns) {
    // [DON-25] fail-close: 모든 패턴이 무효 → 생성자 예외 없이 처리,
    //   check() 는 항상 detected=true 반환
    std::vector<std::string> patterns = {
        "[bad1",
        "[bad2",
    };
    InjectionDetector detector(std::move(patterns));
    // 유효한 패턴 없음 → fail-close → 모든 SQL 차단
    const auto result = detector.check("SELECT 1 UNION SELECT 2");
    EXPECT_TRUE(result.detected)
        << "With all invalid patterns, fail-close should block all SQL (detected=true)";
    EXPECT_EQ(result.reason, "no valid patterns loaded")
        << "reason should indicate no patterns were loaded";
}

// ---------------------------------------------------------------------------
// 탐지 결과 필드 검증
// ---------------------------------------------------------------------------

TEST(InjectionDetector, MatchedPatternField) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT * FROM t UNION SELECT 1,2,3");
    ASSERT_TRUE(result.detected);
    // matched_pattern은 실제 패턴 문자열이어야 함 (로깅/감사용)
    EXPECT_FALSE(result.matched_pattern.empty())
        << "matched_pattern should be set for audit logging";
}

TEST(InjectionDetector, ReasonField) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT SLEEP(5)");
    ASSERT_TRUE(result.detected);
    // reason은 사람이 읽을 수 있는 설명이어야 함
    EXPECT_FALSE(result.reason.empty())
        << "reason should be set for human-readable description";
}

TEST(InjectionDetector, NotDetectedEmptyFields) {
    InjectionDetector detector(default_patterns());
    const auto result = detector.check("SELECT id FROM users WHERE id=1");
    EXPECT_FALSE(result.detected);
    // 탐지 안 됨 → matched_pattern, reason은 빈 문자열
    EXPECT_TRUE(result.matched_pattern.empty());
    EXPECT_TRUE(result.reason.empty());
}

// ---------------------------------------------------------------------------
// [우회 가능성 문서화] — 알려진 미탐을 테스트로 명시
// ---------------------------------------------------------------------------

// [알려진 미탐] 주석 분할 우회: UN/**/ION SEL/**/ECT
// 이 형태는 현재 탐지하지 못한다 (false negative).
// 전처리 단계에서 주석 제거가 필요하나 InjectionDetector 자체는 미구현.
TEST(InjectionDetector, CommentSplitBypass_KnownFalseNegative) {
    InjectionDetector detector(default_patterns());
    // [의도된 false negative] 주석 분할은 탐지 불가
    const auto result = detector.check("UN/**/ION SEL/**/ECT 1,2,3");
    // 이 테스트는 현재 false negative 상태임을 문서화
    // 탐지하지 못하는 것이 현재 알려진 동작
    (void)result;  // 결과에 무관하게 테스트 통과 (한계 문서화 목적)
    SUCCEED() << "Comment-split bypass (UN/**/ION SELECT) is a known false negative. "
                 "Pre-processing comment removal is needed to close this gap.";
}

// [알려진 미탐] 인코딩 우회: 0x55... (hex 리터럴) 또는 CHAR() 함수
TEST(InjectionDetector, EncodingBypass_KnownFalseNegative) {
    InjectionDetector detector(default_patterns());
    // hex 리터럴이나 CHAR() 함수를 통한 우회는 탐지 불가
    const auto result = detector.check(
        "SELECT CHAR(85,78,73,79,78,32,83,69,76,69,67,84)");
    (void)result;
    SUCCEED() << "Encoding bypass via CHAR() is a known false negative. "
                 "String literal evaluation is not in scope.";
}

// main 함수는 test_logger.cpp 에서 제공됨 (단일 dbgate_tests 실행 파일)
