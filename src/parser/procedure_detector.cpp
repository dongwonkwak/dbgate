// ---------------------------------------------------------------------------
// procedure_detector.cpp
//
// CALL/PREPARE/EXECUTE/CREATE PROCEDURE/ALTER PROCEDURE/DROP PROCEDURE 구문
// 탐지 및 동적 SQL 우회 식별 구현.
//
// [탐지 범위]
//   - CALL proc_name(...)           → ProcedureType::kCall, is_dynamic_sql=false
//   - CREATE ... PROCEDURE ...      → ProcedureType::kCreateProcedure
//   - ALTER ... PROCEDURE ...       → ProcedureType::kAlterProcedure
//   - DROP ... PROCEDURE ...        → ProcedureType::kDropProcedure
//   - PREPARE stmt FROM '...'       → ProcedureType::kPrepareExecute, is_dynamic_sql=true
//   - EXECUTE stmt                  → ProcedureType::kPrepareExecute, is_dynamic_sql=true
//
// [알려진 우회 가능성 / 미탐]
// 1. 변수 간접 참조:
//    SET @q = 'DROP TABLE users'; PREPARE s FROM @q; EXECUTE s;
//    → @q 의 실제 값을 추적하지 못한다 (false negative).
//    → is_dynamic_sql=true 는 마킹되므로 정책 엔진이 block_dynamic_sql 로 처리 가능.
// 2. 다중 구문:
//    세미콜론으로 연결된 복수 구문은 첫 번째만 분류됨.
//    "SELECT 1; CALL proc()" 에서 CALL은 탐지하지 못한다 (false negative).
// 3. CALL 프로시저명 우회:
//    CALL /* comment */ proc_name() — 현재 주석 제거를 raw_sql에서 하지 않으므로
//    프로시저명 추출이 실패할 수 있다 (procedure_name="" 반환).
//
// [오탐/미탐 트레이드오프]
// - PREPARE/EXECUTE 자체를 is_dynamic_sql=true로 마킹하면:
//   policy에서 block_dynamic_sql=true 시 합법적인 prepared statement도 차단됨
//   (false positive 증가).
// - PREPARE/EXECUTE를 허용하면: 동적 SQL 우회를 막을 수 없음 (false negative).
// ---------------------------------------------------------------------------

#include "parser/procedure_detector.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

// ---------------------------------------------------------------------------
// 익명 네임스페이스: 내부 헬퍼
// ---------------------------------------------------------------------------
namespace {

// 문자열을 대문자로 변환 (ASCII only)
std::string to_upper_str(const std::string& s) {
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

// raw_sql에서 CALL 뒤의 프로시저 이름을 추출한다.
// 정규식: CALL\s+([\w.]+)\s*\(
// 반환: 프로시저 이름 문자열, 실패 시 빈 문자열
//
// [한계]
// - CALL 과 프로시저명 사이에 주석이 있으면 탐지 실패.
// - schema.proc_name 형태 지원 (점 포함).
std::string extract_procedure_name(const std::string& raw_sql) {
    try {
        // case-insensitive: 원문 SQL에서 직접 추출하여 케이스 보존
        const std::regex call_re(
            "CALL\\s+([\\w.]+)\\s*\\(",
            std::regex_constants::icase | std::regex_constants::ECMAScript
        );

        std::smatch m;
        if (std::regex_search(raw_sql, m, call_re) && m.size() >= 2) {
            return m[1].str();
        }
    } catch (const std::regex_error&) {
        // 정규식 오류는 무시하고 빈 이름 반환
    }

    return {};
}

// raw_sql을 대문자로 변환하여 특정 단어 포함 여부 확인
// word_boundary 적용: 단어 경계 매칭
bool contains_word(const std::string& raw_sql, const std::string& word) {
    const std::string upper = to_upper_str(raw_sql);
    try {
        const std::regex re(
            "\\b" + word + "\\b",
            std::regex_constants::ECMAScript
        );
        return std::regex_search(upper, re);
    } catch (const std::regex_error&) {
        // 폴백: 단순 문자열 탐색
        return upper.find(word) != std::string::npos;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// ProcedureDetector::detect 구현
// ---------------------------------------------------------------------------
std::optional<ProcedureInfo>
ProcedureDetector::detect(const ParsedQuery& query) const {
    switch (query.command) {

        // CALL proc_name(...) 탐지
        case SqlCommand::kCall: {
            const std::string proc_name = extract_procedure_name(query.raw_sql);
            return ProcedureInfo{
                ProcedureType::kCall,
                proc_name,
                false  // CALL 자체는 동적 SQL이 아님
            };
        }

        // CREATE PROCEDURE ... 탐지
        case SqlCommand::kCreate: {
            if (contains_word(query.raw_sql, "PROCEDURE")) {
                return ProcedureInfo{
                    ProcedureType::kCreateProcedure,
                    {},    // procedure_name은 kCall에서만 유효
                    false
                };
            }
            return std::nullopt;
        }

        // ALTER PROCEDURE ... 탐지
        case SqlCommand::kAlter: {
            if (contains_word(query.raw_sql, "PROCEDURE")) {
                return ProcedureInfo{
                    ProcedureType::kAlterProcedure,
                    {},
                    false
                };
            }
            return std::nullopt;
        }

        // DROP PROCEDURE ... 탐지
        case SqlCommand::kDrop: {
            if (contains_word(query.raw_sql, "PROCEDURE")) {
                return ProcedureInfo{
                    ProcedureType::kDropProcedure,
                    {},
                    false
                };
            }
            return std::nullopt;
        }

        // PREPARE / EXECUTE — 동적 SQL 우회 가능성 마킹
        //
        // [보안 설계]
        // PREPARE와 EXECUTE는 문자열 리터럴 내부의 실제 SQL을 파싱하지 않으므로
        // is_dynamic_sql=true로 마킹하여 정책 엔진에 위임.
        // block_dynamic_sql=true 설정 시 차단, false 시 허용 (false positive/negative 트레이드오프).
        //
        // [미탐 주의]
        // SET @q = 'DROP TABLE users'; PREPARE s FROM @q;
        // → @q 의 실제 값 추적 불가. PREPARE/EXECUTE 구문 자체만 탐지.
        case SqlCommand::kPrepare:
        case SqlCommand::kExecute: {
            return ProcedureInfo{
                ProcedureType::kPrepareExecute,
                {},
                true  // 동적 SQL 우회 가능성 있음
            };
        }

        // 그 외: 프로시저/동적 SQL 관련 없음
        default:
            return std::nullopt;
    }
}
