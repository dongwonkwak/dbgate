// ---------------------------------------------------------------------------
// sql_parser.cpp
//
// SQL 구문 분류 및 테이블명 추출 구현.
// "첫 번째 키워드 기반 분류 + 정규식 패턴 매칭" 수준의 경량 파서.
//
// [파서 설계 한계 — 구현 후에도 유지]
// 1. 주석 분할 우회: DROP/**/TABLE 는 블록 주석 제거 후 공백이 삽입되어
//    "DROP  TABLE" 형태가 된다. DROP 키워드는 탐지되나, 인젝션 감지기에서
//    UN/**/ION 같은 분할은 탐지하지 못한다.
//    MySQL 버전 힌트 주석 /*!50000 DROP TABLE */ 은 내용이 제거되어 미탐 가능.
// 2. 인코딩 우회: URL 인코딩, hex 리터럴(0x44524f50='DROP'),
//    멀티바이트 문자 경계 조작은 탐지하지 못한다.
// 3. 복잡한 서브쿼리: SELECT * FROM (SELECT ...) AS t 에서 내부 테이블명은
//    추출하지 않는다.
// 4. Multi-statement: 세미콜론으로 구분된 복수 구문은 첫 번째만 처리.
// 5. PREPARE/EXECUTE 내부 문자열: 리터럴 내부 SQL은 파싱하지 않음.
//    procedure_detector 와 조합하여 탐지해야 한다.
//
// [오탐/미탐 트레이드오프]
// - 보수적(차단 우선) 기본값을 유지한다.
// - 탐지 범위를 넓힐수록 ORM 생성 쿼리에서 false positive 증가.
// - 탐지 범위를 좁힐수록 우회 공격에서 false negative 증가.
// ---------------------------------------------------------------------------

#include "parser/sql_parser.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

// ---------------------------------------------------------------------------
// 익명 네임스페이스: 내부 헬퍼 함수들
// ---------------------------------------------------------------------------
namespace {

// SQL에서 주석을 제거하고 결과 문자열을 반환한다.
//
// 처리 순서:
//   1. /* ... */ 블록 주석 제거 (중첩 미지원)
//   2. -- 인라인 주석 제거 (줄 끝까지)
//   3. # 인라인 주석 제거 (줄 끝까지)
//
// [우회 주의]
// - /*!50000 DROP TABLE */ 형태의 MySQL 조건부 실행 주석은
//   블록 주석으로 처리되어 내용이 제거된다 (false negative).
// - 블록 주석 제거 자리에 공백을 삽입하여 DROP/**/TABLE 이
//   DROPTABLE 로 붙지 않도록 처리한다.
std::string remove_comments(std::string_view sql) {
    std::string result;
    result.reserve(sql.size());

    std::size_t i = 0;
    const std::size_t len = sql.size();

    while (i < len) {
        // 블록 주석 /* ... */
        if (i + 1 < len && sql[i] == '/' && sql[i + 1] == '*') {
            i += 2;
            while (i + 1 < len) {
                if (sql[i] == '*' && sql[i + 1] == '/') {
                    i += 2;
                    break;
                }
                ++i;
            }
            // 블록 주석 자리에 공백 하나 삽입 (DROP/**/ TABLE 처리용)
            result.push_back(' ');
            continue;
        }

        // 인라인 주석 -- (줄 끝까지)
        if (i + 1 < len && sql[i] == '-' && sql[i + 1] == '-') {
            while (i < len && sql[i] != '\n') {
                ++i;
            }
            continue;
        }

        // 해시 주석 # (줄 끝까지)
        if (sql[i] == '#') {
            while (i < len && sql[i] != '\n') {
                ++i;
            }
            continue;
        }

        result.push_back(sql[i]);
        ++i;
    }

    return result;
}

// 문자열을 대문자로 변환한다 (ASCII only).
std::string to_upper(std::string_view s) {
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

// 문자열의 앞뒤 공백(스페이스, 탭, 개행 포함)을 제거한다.
std::string_view trim(std::string_view s) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    const auto begin = std::find_if(s.begin(), s.end(), not_space);
    if (begin == s.end()) {
        return {};
    }
    const auto end = std::find_if(s.rbegin(), s.rend(), not_space).base();
    return s.substr(
        static_cast<std::size_t>(begin - s.begin()),
        static_cast<std::size_t>(end - begin)
    );
}

// 정규화된 SQL(대문자, 주석 제거)에서 첫 번째 키워드를 추출한다.
// 반환값: 첫 번째 공백-구분 토큰, 없으면 빈 문자열
std::string extract_first_keyword(std::string_view normalized_sql) {
    const auto trimmed = trim(normalized_sql);
    if (trimmed.empty()) {
        return {};
    }
    const auto space_pos = trimmed.find_first_of(" \t\r\n");
    if (space_pos == std::string_view::npos) {
        return std::string(trimmed);
    }
    return std::string(trimmed.substr(0, space_pos));
}

// 첫 번째 키워드 → SqlCommand 매핑
SqlCommand keyword_to_command(const std::string& keyword) {
    static const std::unordered_map<std::string, SqlCommand> kKeywordMap = {
        {"SELECT",   SqlCommand::kSelect},
        {"INSERT",   SqlCommand::kInsert},
        {"UPDATE",   SqlCommand::kUpdate},
        {"DELETE",   SqlCommand::kDelete},
        {"DROP",     SqlCommand::kDrop},
        {"TRUNCATE", SqlCommand::kTruncate},
        {"ALTER",    SqlCommand::kAlter},
        {"CREATE",   SqlCommand::kCreate},
        {"CALL",     SqlCommand::kCall},
        {"PREPARE",  SqlCommand::kPrepare},
        {"EXECUTE",  SqlCommand::kExecute},
    };

    const auto it = kKeywordMap.find(keyword);
    if (it != kKeywordMap.end()) {
        return it->second;
    }
    return SqlCommand::kUnknown;
}

// 테이블명으로 허용되는 문자인지 확인한다.
// 허용: 알파벳, 숫자, 밑줄, 점(schema.table), 백틱
bool is_table_name_char(char c) {
    return (std::isalnum(static_cast<unsigned char>(c)) != 0)
        || c == '_'
        || c == '.'
        || c == '`';
}

// normalized_sql에서 keyword 뒤에 오는 테이블명(들)을 추출하여 out_tables에 추가.
// 쉼표 구분 복수 테이블 지원 (FROM t1, t2).
//
// [우회 주의]
// - "FROM (SELECT ...)" 처럼 서브쿼리가 오면 '(' 를 테이블명으로 잘못 인식하지 않도록
//   괄호로 시작하는 토큰은 건너뜀.
// - "FROM t1 AS a, t2" 처럼 별칭이 오면 별칭도 함께 잡힐 수 있음.
//   현재는 첫 토큰만 추출하여 별칭 오탐을 최소화.
// - ORM 생성 쿼리에서 false positive 발생 가능 (알려진 한계).
void extract_tables_for_keyword(
    const std::string&        normalized_sql,
    std::string_view          original_sql,
    const std::string&        keyword,
    std::vector<std::string>& out_tables
) {
    // keyword 다음에 오는 테이블명(들)을 추출하는 정규식
    // 쉼표 구분 복수 테이블: FROM t1, t2, t3
    // 각 테이블명은 백틱 선택적 포함
    const std::string pattern =
        keyword + "\\s+(`?[\\w.]+`?(?:\\s*,\\s*`?[\\w.]+`?)*)";

    try {
        std::regex re(pattern,
                      std::regex_constants::icase |
                      std::regex_constants::ECMAScript);

        auto it = std::sregex_iterator(
            normalized_sql.begin(), normalized_sql.end(), re);
        const auto end_it = std::sregex_iterator();

        for (; it != end_it; ++it) {
            const std::smatch& m = *it;
            if (m.size() < 2) {
                continue;
            }

            std::string table_list = m[1].str();

            // 쉼표로 분리하여 각 테이블명 처리
            std::size_t pos = 0;
            while (pos <= table_list.size()) {
                // 앞 공백 건너뜀
                while (pos < table_list.size() &&
                       std::isspace(static_cast<unsigned char>(table_list[pos])) != 0) {
                    ++pos;
                }
                if (pos >= table_list.size()) {
                    break;
                }

                // 쉼표 찾기
                const auto comma = table_list.find(',', pos);
                std::string token;
                if (comma == std::string::npos) {
                    token = table_list.substr(pos);
                    pos = table_list.size() + 1;
                } else {
                    token = table_list.substr(pos, comma - pos);
                    pos = comma + 1;
                }

                // 토큰 앞뒤 공백 제거
                const auto trimmed_sv = trim(token);
                if (trimmed_sv.empty()) {
                    continue;
                }
                std::string trimmed_token(trimmed_sv);

                // 백틱 제거
                if (!trimmed_token.empty() && trimmed_token.front() == '`') {
                    trimmed_token.erase(0, 1);
                }
                if (!trimmed_token.empty() && trimmed_token.back() == '`') {
                    trimmed_token.pop_back();
                }

                // 서브쿼리 시작 '(' 건너뜀
                if (trimmed_token.empty() || trimmed_token.front() == '(') {
                    continue;
                }

                // 원문 SQL에서 케이스 보존된 이름 추출
                const std::string upper_token = to_upper(trimmed_token);
                const std::string orig_str(original_sql);
                const std::string orig_upper = to_upper(orig_str);

                std::string final_name = trimmed_token;

                // 원문에서 케이스 보존 추출 시도 (단어 경계 확인)
                std::size_t search_from = 0;
                while (search_from < orig_upper.size()) {
                    const auto found = orig_upper.find(upper_token, search_from);
                    if (found == std::string::npos) {
                        break;
                    }

                    // 단어 경계 확인: 앞뒤가 식별자 문자가 아니어야 함
                    const bool valid_start =
                        (found == 0) ||
                        (!is_table_name_char(orig_str[found - 1]));
                    const bool valid_end =
                        (found + upper_token.size() >= orig_str.size()) ||
                        (!is_table_name_char(orig_str[found + upper_token.size()]));

                    if (valid_start && valid_end) {
                        final_name = orig_str.substr(found, upper_token.size());
                        break;
                    }
                    search_from = found + 1;
                }

                // 중복 추가 방지 (대소문자 무관)
                const auto upper_final = to_upper(final_name);
                bool already_added = false;
                for (const auto& t : out_tables) {
                    if (to_upper(t) == upper_final) {
                        already_added = true;
                        break;
                    }
                }
                if (!already_added) {
                    out_tables.push_back(std::move(final_name));
                }
            }
        }
    } catch (const std::regex_error& e) {
        spdlog::warn("sql_parser: regex error for keyword '{}': {}", keyword, e.what());
    }
}

// 정규화된 SQL에서 "WHERE" 단어 포함 여부 확인
// 단어 경계 적용: ELSEWHERE 같은 단어에서 오탐 방지
bool has_where_keyword(const std::string& normalized_sql) {
    try {
        const std::regex where_re(
            "\\bWHERE\\b",
            std::regex_constants::ECMAScript
        );
        return std::regex_search(normalized_sql, where_re);
    } catch (const std::regex_error&) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// has_semicolon_outside_string_or_comment
//   원문 SQL에서 문자열 리터럴(' 또는 " 감쌈)과 주석(/* */, --, #) 밖에
//   세미콜론이 존재하면 true 를 반환한다.
//
//   이 함수는 주석 제거 전 원문을 직접 스캔하여 멀티 스테이트먼트를 감지한다.
//   주석 제거 후에는 주석 내 세미콜론도 사라지므로 false negative 위험이 있어
//   원문 스캔 방식을 채택한다.
//
// [한계 / 알려진 우회 가능성]
// - 중첩 주석 미지원 (MySQL은 중첩 블록 주석을 허용하지 않으므로 실제 영향 미미).
// - 백슬래시 이스케이프 처리 불완전: '\'' 같은 이스케이프 시퀀스는
//   상태 머신 방식으로 처리하여 오탐을 방지한다.
// - 큰따옴표 문자열(ANSI mode)도 처리하나, MySQL 기본 모드에서는
//   큰따옴표가 식별자 구분자로 사용될 수 있어 일부 false negative 가능.
//
// [오탐/미탐 트레이드오프]
// - 보수적(차단 우선): 파싱 불확실 시 세미콜론이 있다고 판정한다.
// ---------------------------------------------------------------------------
bool has_semicolon_outside_string_or_comment(std::string_view sql) {
    // 상태 머신으로 문자열/주석 영역을 추적
    enum class State : std::uint8_t {
        kNormal,         // 일반 SQL 구문
        kSingleQuote,    // 'string' 내부
        kDoubleQuote,    // "string" 내부
        kBlockComment,   // /* ... */ 내부
        kLineComment,    // -- ... \n 내부
        kHashComment,    // # ... \n 내부
    };

    State state = State::kNormal;
    const std::size_t len = sql.size();

    for (std::size_t i = 0; i < len; ++i) {
        const char c = sql[i];
        const char next = (i + 1 < len) ? sql[i + 1] : '\0';

        switch (state) {
            case State::kNormal:
                if (c == '\'') {
                    state = State::kSingleQuote;
                } else if (c == '"') {
                    state = State::kDoubleQuote;
                } else if (c == '/' && next == '*') {
                    state = State::kBlockComment;
                    ++i;  // '*' 건너뜀
                } else if (c == '-' && next == '-') {
                    state = State::kLineComment;
                    ++i;  // 두 번째 '-' 건너뜀
                } else if (c == '#') {
                    state = State::kHashComment;
                } else if (c == ';') {
                    // 문자열/주석 밖 세미콜론 발견 → 멀티 스테이트먼트
                    return true;
                }
                break;

            case State::kSingleQuote:
                if (c == '\\') {
                    // 이스케이프 문자: 다음 문자 건너뜀 (\' 등)
                    ++i;
                } else if (c == '\'') {
                    if (next == '\'') {
                        // '' 이스케이프: 두 번째 따옴표 건너뜀
                        ++i;
                    } else {
                        state = State::kNormal;
                    }
                }
                break;

            case State::kDoubleQuote:
                if (c == '\\') {
                    ++i;
                } else if (c == '"') {
                    if (next == '"') {
                        ++i;
                    } else {
                        state = State::kNormal;
                    }
                }
                break;

            case State::kBlockComment:
                if (c == '*' && next == '/') {
                    state = State::kNormal;
                    ++i;  // '/' 건너뜀
                }
                break;

            case State::kLineComment:
            case State::kHashComment:
                if (c == '\n') {
                    state = State::kNormal;
                }
                break;
        }
    }

    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// SqlParser::parse 구현
// ---------------------------------------------------------------------------
std::expected<ParsedQuery, ParseError>
SqlParser::parse(std::string_view sql) const {
    // 1. 빈 입력 검사
    const auto trimmed = trim(sql);
    if (trimmed.empty()) {
        return std::unexpected(ParseError{
            ParseErrorCode::kInvalidSql,
            "Empty SQL input",
            std::string(sql)
        });
    }

    // 2. 멀티 스테이트먼트 감지 (주석 제거 전 원문에서 수행)
    //
    // [보안 원칙] 문자열/주석 외부에 세미콜론이 있으면 fail-close(ParseError 반환).
    // 멀티 스테이트먼트 SQL은 piggyback 공격의 주요 벡터이므로 파싱 단계에서 차단.
    //
    // [오탐 가능성] 세미콜론으로 끝나는 단일 구문 (예: "SELECT 1;")도 탐지됨.
    //   → 보수적 설계. 단일 구문 끝 세미콜론은 사전 전처리로 제거 권고.
    if (has_semicolon_outside_string_or_comment(sql)) {
        spdlog::warn("sql_parser: multi-statement detected (semicolon outside string/comment), "
                     "fail-close applied. sql_prefix='{}'",
                     std::string(sql).substr(0, 80));
        return std::unexpected(ParseError{
            ParseErrorCode::kInvalidSql,
            "Multi-statement SQL detected: semicolon outside string or comment",
            std::string(sql)
        });
    }

    // 3. 주석 제거
    const std::string no_comments = remove_comments(sql);

    // 4. 대문자 정규화
    const std::string normalized = to_upper(no_comments);

    // 정규화 후 공백만 남은 경우
    const auto normalized_trimmed = trim(normalized);
    if (normalized_trimmed.empty()) {
        return std::unexpected(ParseError{
            ParseErrorCode::kInvalidSql,
            "SQL is empty after comment removal",
            std::string(sql)
        });
    }

    // 5. 첫 번째 키워드로 SqlCommand 분류
    const std::string first_kw = extract_first_keyword(normalized_trimmed);
    const SqlCommand cmd = keyword_to_command(first_kw);

    // 6. 테이블명 추출
    // command에 따라 탐색할 키워드가 다름
    std::vector<std::string> tables;

    const std::string normalized_str(normalized_trimmed);

    switch (cmd) {
        case SqlCommand::kSelect:
        case SqlCommand::kDelete:
            extract_tables_for_keyword(normalized_str, sql, "FROM", tables);
            extract_tables_for_keyword(normalized_str, sql, "JOIN", tables);
            break;

        case SqlCommand::kInsert:
            extract_tables_for_keyword(normalized_str, sql, "INTO", tables);
            break;

        case SqlCommand::kUpdate:
            extract_tables_for_keyword(normalized_str, sql, "UPDATE", tables);
            extract_tables_for_keyword(normalized_str, sql, "JOIN", tables);
            break;

        case SqlCommand::kDrop:
        case SqlCommand::kTruncate:
            // "DROP TABLE users" / "TRUNCATE TABLE users"
            extract_tables_for_keyword(normalized_str, sql, "TABLE", tables);
            break;

        case SqlCommand::kAlter:
        case SqlCommand::kCreate:
            extract_tables_for_keyword(normalized_str, sql, "TABLE", tables);
            break;

        case SqlCommand::kCall:
        case SqlCommand::kPrepare:
        case SqlCommand::kExecute:
        case SqlCommand::kUnknown:
        default:
            // 테이블명 추출 불필요 또는 불가
            break;
    }

    // 7. has_where_clause 판정
    const bool has_where = has_where_keyword(normalized_str);

    // 8. ParsedQuery 구성
    // raw_sql은 원문 그대로 보존
    ParsedQuery result;
    result.command          = cmd;
    result.tables           = std::move(tables);
    result.raw_sql          = std::string(sql);
    result.has_where_clause = has_where;

    return result;
}
