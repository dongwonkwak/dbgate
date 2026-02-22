#pragma once

// ---------------------------------------------------------------------------
// procedure_detector.hpp
//
// CALL/PREPARE/EXECUTE/CREATE PROCEDURE/ALTER PROCEDURE/DROP PROCEDURE 구문을
// 탐지하고 동적 SQL 우회 시도를 식별한다.
//
// [설계 한계 / 알려진 우회 가능성]
// 1. PREPARE/EXECUTE 를 통한 동적 SQL 우회:
//    PREPARE stmt FROM 'DROP TABLE users'; EXECUTE stmt;
//    => is_dynamic_sql=true 로 마킹하여 정책 엔진에 위임.
//    단, 문자열 리터럴 내부의 실제 SQL 내용은 파싱하지 않는다.
// 2. 변수 간접 참조:
//    SET @q = 'DROP TABLE users'; PREPARE s FROM @q; EXECUTE s;
//    => @q 의 실제 값을 추적하지 못한다 (false negative 가능).
// 3. 다중 구문(multi-statement): 세미콜론으로 연결된 복수 구문은
//    첫 번째 구문만 분류된다.
//
// [오탐/미탐 트레이드오프]
// - block_dynamic_sql=true 설정 시: PREPARE/EXECUTE 자체를 차단하므로
//   합법적인 prepared statement 사용도 차단된다 (false positive).
// - block_dynamic_sql=false 설정 시: 동적 SQL 우회를 허용한다 (false negative).
// ---------------------------------------------------------------------------

#include <optional>
#include <string>

#include "sql_parser.hpp"  // ParsedQuery, SqlCommand

// ---------------------------------------------------------------------------
// ProcedureType
//   감지된 프로시저/동적 SQL 구문 종류.
// ---------------------------------------------------------------------------
enum class ProcedureType : std::uint8_t {
    kCall             = 0,  // CALL proc_name(...)
    kCreateProcedure  = 1,  // CREATE PROCEDURE ...
    kAlterProcedure   = 2,  // ALTER PROCEDURE ...
    kDropProcedure    = 3,  // DROP PROCEDURE ...
    kPrepareExecute   = 4,  // PREPARE ... FROM ... 또는 EXECUTE ...
};

// ---------------------------------------------------------------------------
// ProcedureInfo
//   프로시저 탐지 결과.
//   procedure_name 은 ProcedureType::kCall 일 때만 유효하다.
// ---------------------------------------------------------------------------
struct ProcedureInfo {
    ProcedureType type{ProcedureType::kCall};

    // CALL proc_name(...) 에서 추출한 프로시저 이름.
    // kCall 이외의 type 에서는 비어 있을 수 있다.
    std::string procedure_name{};

    // PREPARE/EXECUTE 구문 여부.
    // true 이면 동적 SQL 우회 가능성이 있음을 의미한다.
    // [주의] 실제 동적 SQL 내용(문자열 리터럴 내부)은 검사하지 않는다.
    bool is_dynamic_sql{false};
};

// ---------------------------------------------------------------------------
// ProcedureDetector
//   ParsedQuery 를 받아 프로시저/동적 SQL 관련 정보를 반환한다.
//   탐지 대상이 아닌 구문이면 std::nullopt 를 반환한다.
//
//   [사용 예]
//   auto info = detector.detect(parsed_query);
//   if (info && info->is_dynamic_sql) {
//       // 정책 엔진에 동적 SQL 우회 가능성 전달
//   }
// ---------------------------------------------------------------------------
class ProcedureDetector {
public:
    ProcedureDetector()  = default;
    ~ProcedureDetector() = default;

    ProcedureDetector(const ProcedureDetector&)            = default;
    ProcedureDetector& operator=(const ProcedureDetector&) = default;
    ProcedureDetector(ProcedureDetector&&)                 = default;
    ProcedureDetector& operator=(ProcedureDetector&&)      = default;

    // detect
    //   query: SqlParser::parse 의 결과 ParsedQuery
    //   반환:  프로시저/동적 SQL 탐지 결과, 해당 없으면 nullopt
    //
    // [미탐 주의]
    // SET @q = '...'; PREPARE s FROM @q; 패턴에서 @q 의 실제 값이
    // 위험한 SQL 이어도 탐지하지 못한다.
    [[nodiscard]] std::optional<ProcedureInfo>
    detect(const ParsedQuery& query) const;
};
