# ADR-006: SQL 파서: 풀 파서 대신 키워드+정규식 선택

## Status

Accepted

## Context

dbgate는 MySQL 클라이언트와 서버 사이에서 SQL을 실시간으로 검사해야 한다.
검사 레이어에서 SQL을 해석하는 방법으로 두 가지를 검토했다:

1. **풀 SQL 파서**: yacc/flex 기반 완전한 SQL 문법 파서 (예: MySQL의 sql_yacc.yy 수준)
2. **키워드+정규식 경량 파서**: 첫 번째 키워드 기반 구문 분류 + 정규식 패턴 매칭

## Decision

**키워드+정규식 기반 경량 파서**를 선택한다.

구체적으로:
- 주석 전처리: `-- ...`, `/* ... */`, `# ...` 제거 후 대문자 정규화
- 구문 분류: 첫 번째 키워드(`SELECT`, `INSERT`, ...) 기반 `SqlCommand` 분류
- 테이블명 추출: `FROM`, `INTO`, `UPDATE`, `JOIN`, `TABLE` 뒤의 식별자 정규식 추출
- SQL Injection 탐지: `InjectionDetector`에서 별도의 `std::regex` 패턴 매칭
- 프로시저 탐지: `ProcedureDetector`에서 `ParsedQuery` 기반 분류

## Rationale (선택 이유)

### 빌드 복잡성 최소화
- yacc/flex는 별도 툴체인(flex, bison), 생성 코드 관리, CI/CD 통합이 필요하다.
- `std::regex` + 문자열 처리는 C++23 표준 라이브러리만으로 구현 가능하다.
- 의존성 추가 없이 GCC 14 + CMake 환경에서 바로 빌드된다.

### 성능
- 풀 파서는 전체 SQL 문법을 파싱하므로 복잡한 쿼리에서 레이턴시가 높다.
- 경량 파서는 첫 번째 키워드 분류 + O(P * N) 정규식 매칭으로 실시간 처리에 적합하다.
- DB 프록시의 핵심 성능 경로(hot path)에서 파싱 비용을 최소화한다.

### 유지보수성
- SQL 문법은 데이터베이스마다 다르고(MySQL, PostgreSQL, MariaDB), 풀 파서는
  각 방언에 대한 별도 구현이 필요하다.
- 정책 기반 차단에는 완전한 AST가 필요하지 않고, 구문 분류 + 테이블명 + 인젝션 패턴이면 충분하다.
- 정규식 패턴은 config에서 로드하여 재컴파일 없이 운영 중 변경 가능하다.

## Consequences

### Positive

- **단순성**: 표준 라이브러리만 사용. 외부 파서 툴체인 불필요.
- **예측 가능성**: 정규식 패턴이 명시적이고 감사(audit) 가능하다.
- **Hot Reload 친화성**: 정규식 패턴을 YAML 정책 파일에서 로드하므로 재시작 없이 변경 가능.
- **Fail-close 보장**: 파싱 실패 시 `kInternalError` 반환 → 정책 엔진에서 `kBlock` 처리.

### Negative (알려진 한계)

1. **서브쿼리 테이블명 미추출**
   - `SELECT * FROM (SELECT id FROM inner_table) AS t`
   - `inner_table`은 추출하지 않는다.
   - 영향: 테이블 기반 접근 제어가 서브쿼리 내부 테이블에는 적용되지 않는다.

2. **주석 분할 우회 (false negative)**
   - `DROP/**/TABLE users`: 블록 주석 제거 후 `DROP  TABLE users` → DROP 키워드 탐지됨.
   - `UN/**/ION SEL/**/ECT`: `InjectionDetector` 수준에서 탐지 불가.
   - MySQL 버전 힌트 주석 `/*!50000 DROP TABLE */`: 내용이 제거되어 미탐 가능.
   - 완화: 전처리 단계에서 주석을 제거하여 일부 우회 차단.

3. **인코딩 우회 (false negative)**
   - URL 인코딩: `%53%45%4C%45%43%54` → 탐지 불가.
   - Hex 리터럴: `SELECT 0x44524f50` → 탐지 불가.
   - `CHAR()` 함수: `SELECT CHAR(83,69,76,69,67,84)` → 탐지 불가.
   - 완화: 네트워크 레이어에서 URL 디코딩을 적용하면 일부 해소.

4. **Multi-statement (false negative)**
   - `SELECT 1; DROP TABLE users`: 첫 번째 구문(`SELECT`)만 분류.
   - `InjectionDetector`의 피기백 패턴(`;...DROP`)으로 부분 완화.

5. **ORM 쿼리 false positive**
   - Hibernate, SQLAlchemy 등 ORM이 생성하는 복잡한 `UNION ALL` 페이징 쿼리에서
     `UNION SELECT` 패턴이 오탐될 수 있다.
   - 완화: 정규식 패턴을 운영자가 config에서 튜닝 가능.

## False Positive vs False Negative 트레이드오프

| 설정 방향 | False Positive (오탐) | False Negative (미탐) | 권장 상황 |
|----------|----------------------|----------------------|---------|
| 패턴 확대 | 증가 (ORM 쿼리 차단 가능) | 감소 | 보안 우선 환경 |
| 패턴 축소 | 감소 | 증가 (우회 허용) | 서비스 가용성 우선 환경 |
| **현재 기본값** | **보수적 (차단 우선)** | **일부 우회 가능** | **대부분의 운영 환경** |

**원칙**: fail-close. 탐지 불확실 시에도 정책 엔진이 `kBlock`을 기본값으로 적용한다.

## Alternatives Considered

### Option A: yacc/flex 풀 SQL 파서

- **장점**: AST 수준 분석, 서브쿼리/조인 완전 파싱, 인코딩 우회 저항성
- **단점**: 빌드 복잡성 증가, MySQL 방언별 별도 구현 필요, 레이턴시 증가
- **결론**: 현 단계에서 오버엔지니어링. 보안 요구사항이 변경되면 재검토.

### Option B: 외부 파서 라이브러리 (예: libmysqlclient 활용)

- **장점**: MySQL 공식 파서 활용 → 높은 정확도
- **단점**: 라이선스 제약(GPL), 버전 의존성, 빌드 복잡성
- **결론**: 라이선스 및 의존성 문제로 제외.

### Option C: 현재 선택 (키워드+정규식)

- **장점**: 단순, 빠름, 유지보수 용이, 정책 Hot Reload 친화적
- **단점**: 위 알려진 한계
- **결론**: 현재 보안 요구사항에 적합하며, 한계를 명시적으로 문서화한다.

## References

- `src/parser/sql_parser.hpp` — 파서 설계 한계 주석
- `src/parser/injection_detector.hpp` — 오탐/미탐 트레이드오프 주석
- `src/parser/procedure_detector.hpp` — 동적 SQL 우회 한계 주석
- `docs/interface-reference.md` — 파서 인터페이스 레퍼런스
