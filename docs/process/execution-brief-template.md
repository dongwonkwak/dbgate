# Execution Brief 템플릿 v1

## 목적
- Claude App(Architect)이 Claude Code 서브에이전트에 전달할 **구조화된 실행 지시서**.
- Linear 이슈 코멘트에 사본을 남겨 계획 → 실행 → 결과의 추적성을 확보한다.
- Handoff Report와 쌍을 이루어, 입력(Brief)과 출력(Report)을 연결한다.

## 적용 기준

### 표준 Brief (Full)
외부 영향이 있는 변경에 사용한다:
- `behavior`, `interface`, `ops`, `perf`
- 다중 에이전트 참여
- 테스트/문서 영향이 예상되는 작업

### 경량 Brief (Light)
외부 영향이 없는 변경에 사용한다:
- `internal-refactor`, `docs-only`
- 단일 에이전트가 담당 경로 내에서 완결
- 범위가 명확하고 좁은 bug fix

경량 Brief는 `[선택]` 표시된 섹션을 생략할 수 있다.

## 운영 규칙
1. Claude App에서 Brief를 작성한다.
2. Linear 이슈 코멘트에 아래 형식으로 남긴다:
   - `📋 Execution Brief v<N> — DON-XX <작업요약>`
3. Claude Code 실행 시 Brief를 프롬프트로 전달한다.
4. 재작업/범위 변경 시 버전을 올려 새 코멘트로 남긴다 (v1 → v2). 기존 코멘트는 덮어쓰지 않는다.
5. Handoff Report에 사용한 Brief 버전을 기록한다.

---

## 표준 Brief 템플릿

```markdown
📋 Execution Brief v1 — DON-XX <작업요약>

### 메타
- Linear: DON-XX
- 브랜치: feat/DON-XX-<module> 또는 fix/DON-XX-<module>
- 변경 유형: behavior | interface | ops | perf
- 대상 에이전트: network-engineer | security-engineer | infra-engineer | go-engineer | qa-engineer | technical-writer
- Architect 승인 필요: 예/아니오

### 목표 (Goal)
- 이 작업이 왜 필요한지, 무엇을 달성해야 하는지.

### 범위 (In Scope)
- 수정 대상 파일/모듈

### 제외 범위 (Out of Scope)
- 건드리지 말아야 할 파일/모듈/인터페이스

### 제약
- CLAUDE.md, 기존 인터페이스, 설계 원칙 중 이 작업에 특히 관련된 것.
- 의존하는 선행 작업 (예: DON-XX 완료 전제).

### 구현 지시
- Claude Code 서브에이전트가 실행할 구체적 작업 내용.
- 구현 순서, 주의사항, 참조해야 할 기존 코드/문서.

### 완료 조건 (Acceptance Criteria)
- [ ] 조건 1
- [ ] 조건 2
- [ ] 빌드/테스트 통과

### 테스트 계획
- 추가/수정할 테스트
- 검증 명령어 (예: `cmake --build build --target test`)

### Doc Impact 예상
- 영향 문서 후보 (ownership-map 기준)
- 문서 수정 필요 여부 및 사유

### 리뷰 포인트
- 주요 리스크 (보안/성능/호환성)
- 엣지 케이스
- 회귀 가능성

### [선택] Open Questions
- 실행 전 확인이 필요한 미결 사항
```

---

## 경량 Brief 템플릿

```markdown
📋 Execution Brief v1 (Light) — DON-XX <작업요약>

### 메타
- Linear: DON-XX
- 브랜치: feat/DON-XX-<module> 또는 fix/DON-XX-<module>
- 변경 유형: internal-refactor | docs-only | bug-fix(경량)
- 대상 에이전트: <해당 에이전트>

### 목표
- 한 줄 요약.

### 범위
- Touch: 수정 대상
- Do Not Touch: 건드리지 말 것 (해당 시)

### 구현 지시
- 수행할 작업 내용.

### 완료 조건
- [ ] 조건 1
- [ ] 빌드/테스트 통과

### Doc Impact
- none — 사유: <한 줄>

### [선택] 주의할 회귀 포인트
- 해당 시 기재
```

---

## 예시: 표준 Brief

```markdown
📋 Execution Brief v1 — DON-28 Boost.Asio 프록시 서버 모듈 통합

### 메타
- Linear: DON-28
- 브랜치: feat/DON-28-proxy-server
- 변경 유형: behavior, interface
- 대상 에이전트: network-engineer (주), qa-engineer (병렬)
- Architect 승인 필요: 예 (모듈 통합으로 인터페이스 조정 가능)

### 목표 (Goal)
- Boost.Asio 기반 프록시 서버에 DON-24/25/26/27 모듈을 통합하여,
  mysql CLI로 프록시 경유 접속 → COM_QUERY 파싱 → 정책 판정 → 릴레이/차단이 동작하도록 한다.

### 범위 (In Scope)
- src/proxy/, src/main.cpp, config/proxy.yaml

### 제외 범위 (Out of Scope)
- src/parser/, src/policy/ (기존 인터페이스 그대로 사용)
- tools/ (Go 컨트롤플레인은 별도 이슈)

### 제약
- 핸드셰이크 패스스루 원칙 유지 (ADR-002)
- 1:1 세션 릴레이 (커넥션 풀링 금지)
- Fail-close: 파싱/정책 오류 시 반드시 차단
- DON-24, DON-25, DON-26, DON-27 완료 전제

### 구현 지시
1. src/proxy/server.cpp에 Boost.Asio acceptor 구현 (co_await 기반)
2. 세션 생성 시 protocol → parser → policy → logger 순서로 모듈 조립
3. COM_QUERY 경로에서 PolicyEngine::evaluate() 호출, BLOCK 시 MySQL Error Packet 반환
4. Graceful Shutdown: SIGTERM 수신 시 신규 연결 거부, 기존 세션 완료 대기
5. Health Check: /health 엔드포인트 (TCP)
6. 참조: src/protocol/handshake.hpp, src/parser/sql_parser.hpp, src/policy/policy_engine.hpp

### 완료 조건 (Acceptance Criteria)
- [ ] mysql CLI로 프록시 경유 접속 성공
- [ ] DROP TABLE 차단 확인
- [ ] SQL Injection 차단 확인
- [ ] Graceful Shutdown 동작
- [ ] 빌드/테스트 통과

### 테스트 계획
- 통합 테스트: 정상 쿼리 릴레이, 차단 쿼리 에러 반환
- 엣지 케이스: malformed packet, 대량 연결, 타임아웃
- 검증: cmake --build build --target test

### Doc Impact 예상
- docs/architecture.md, docs/data-flow.md, docs/sequences.md 업데이트 필요
- docs/interface-reference.md 통합 후 변경 사항 반영

### 리뷰 포인트
- 세션 종료 경로에서 리소스 누수 없는지
- fail-close 경로가 모든 에러 케이스를 커버하는지
- 동시 연결 시 스레드 안전성

### Open Questions
- Health Check 엔드포인트 경로 확정 필요 (/health? /healthz?)
- 정책 Hot Reload 시 기존 세션에 미치는 영향 범위
```

---

## 예시: 경량 Brief

```markdown
📋 Execution Brief v1 (Light) — DON-35 logger 내부 중복 정리

### 메타
- Linear: DON-35
- 브랜치: fix/DON-35-logger-cleanup
- 변경 유형: internal-refactor
- 대상 에이전트: infra-engineer

### 목표
- logger 모듈 내부 중복 코드 정리 (동작 변화 없음).

### 범위
- Touch: src/logger/structured_logger.cpp, src/logger/log_formatter.cpp
- Do Not Touch: src/logger/log_types.hpp (인터페이스 변경 없음)

### 구현 지시
- log_formatter.cpp의 중복 포맷 함수를 공통 헬퍼로 추출
- structured_logger.cpp에서 추출된 헬퍼 호출로 전환

### 완료 조건
- [ ] 기존 테스트 전부 통과
- [ ] 빌드 성공

### Doc Impact
- none — 사유: 내부 구조 정리만, 동작/인터페이스/운영 변화 없음
```

---

## 관련 문서
- `docs/process/handoff-report-schema.md` — Handoff Report에 Brief 버전 기록
- `docs/process/state-transition-checklist.md` — 상태 전이 시 Brief 존재 확인
- `docs/process/agent-workflow.md` — 전체 작업 흐름
- `.claude/ownership-map.yaml` — 경로별 문서 후보 매핑