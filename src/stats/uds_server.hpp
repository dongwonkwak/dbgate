#pragma once

// ---------------------------------------------------------------------------
// uds_server.hpp
//
// Unix Domain Socket 서버. Go CLI/대시보드에 통계를 노출한다.
//
// [프로토콜: 길이 프리픽스 + JSON]
//   요청 프레임:
//     [4byte LE 길이][JSON 본문]
//     예: {"command": "stats", "version": 1}
//
//   응답 프레임:
//     [4byte LE 길이][JSON 본문]
//     성공: {"ok": true,  "payload": { ...StatsSnapshot 필드... }}
//     실패: {"ok": false, "error": "<메시지>"}
//
// [지원 커맨드]
//   "stats"          — StatsSnapshot 반환
//   "policy_explain" — SQL 정책 평가 dry-run (DON-48)
//   "sessions"       — 활성 세션 목록 (Phase 3 확장)
//   "policy_reload"  — 정책 리로드 트리거 (Phase 3 확장)
//
// [버전 관리]
//   CommandRequest.version 필드로 프로토콜 버전 구분.
//   현재 버전: 1. 미지정 시 기본값 1 적용.
//
// [스레드/비동기 모델]
//   Boost.Asio co_await 기반. io_context 는 외부에서 주입.
//   run() 은 co_return 까지 accept 루프를 유지한다.
//   stop() 은 acceptor 를 닫아 run() 을 종료시킨다.
//
// [격리 원칙]
//   UDS I/O 실패가 데이터패스 실패로 전파되지 않도록
//   stats 접근은 read-only (StatsCollector::snapshot()) 만 수행한다.
//   policy_explain 실패는 데이터패스로 전파하지 않는다.
// ---------------------------------------------------------------------------

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "parser/sql_parser.hpp"
#include "policy/policy_engine.hpp"
#include "stats_collector.hpp"

namespace asio = boost::asio;

// ---------------------------------------------------------------------------
// UdsServer
//   StatsCollector 의 snapshot() 및 policy_explain 을 UDS 클라이언트에 노출.
// ---------------------------------------------------------------------------
class UdsServer {
public:
    // 생성자 (stats 전용 — policy_explain 비활성)
    //   socket_path : Unix Domain Socket 파일 경로
    //   stats       : 공유 통계 수집기 (read-only 접근만 수행)
    //   ioc         : 외부에서 주입된 Asio io_context
    UdsServer(const std::filesystem::path& socket_path,
              std::shared_ptr<StatsCollector> stats,
              asio::io_context& ioc);

    // 생성자 (policy_explain 활성)
    //   policy_engine : 정책 엔진 (explain dry-run 전용; nullptr 이면 policy_explain 비활성)
    //   sql_parser    : SQL 파서 (nullptr 이면 policy_explain 비활성)
    UdsServer(const std::filesystem::path& socket_path,
              std::shared_ptr<StatsCollector> stats,
              std::shared_ptr<PolicyEngine> policy_engine,
              std::shared_ptr<SqlParser> sql_parser,
              asio::io_context& ioc);

    ~UdsServer();

    // 복사 금지
    UdsServer(const UdsServer&) = delete;
    UdsServer& operator=(const UdsServer&) = delete;

    // 이동 금지 (acceptor 소유권 명확화)
    UdsServer(UdsServer&&) = delete;
    UdsServer& operator=(UdsServer&&) = delete;

    // run
    //   UDS 소켓 바인드/리슨 후 accept 루프를 실행한다.
    //   호출자는 co_await 로 이 코루틴을 구동해야 한다.
    asio::awaitable<void> run();

    // stop
    //   acceptor 를 닫아 run() 의 accept 루프를 종료한다.
    //   io_context 스레드에서 안전하게 호출 가능.
    void stop();

private:
    // handle_client
    //   단일 클라이언트 연결을 처리하는 코루틴.
    //   요청 JSON 파싱 → 커맨드 디스패치 → 응답 직렬화 → 송신.
    asio::awaitable<void> handle_client(asio::local::stream_protocol::socket socket);

    // handle_policy_explain
    //   "policy_explain" 커맨드 처리.
    //   payload에서 sql/user/source_ip 파싱 후 PolicyEngine::explain() 또는
    //   explain_error() 를 호출하고 JSON 응답 문자열을 반환한다.
    //   payload 필드 누락/파싱 실패 시 {"ok":false,"error":"..."} 반환 (fail-close).
    //   policy_engine_ 또는 sql_parser_ 가 nullptr 이면 not-implemented 응답 반환.
    [[nodiscard]] std::string handle_policy_explain(std::string_view request_json);

    std::filesystem::path socket_path_;
    std::shared_ptr<StatsCollector> stats_;
    std::shared_ptr<PolicyEngine> policy_engine_;  // nullable
    std::shared_ptr<SqlParser> sql_parser_;        // nullable
    asio::io_context& ioc_;
    asio::local::stream_protocol::acceptor acceptor_;
    std::atomic<bool> stop_requested_{false};
};
