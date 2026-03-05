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
//   "stats"           — StatsSnapshot 반환
//   "policy_explain"  — SQL 정책 평가 dry-run (DON-48)
//   "policy_versions" — 저장된 정책 버전 목록 조회 (DON-50)
//   "policy_rollback" — 특정 버전으로 정책 롤백 (DON-50)
//   "policy_reload"   — 정책 파일 리로드 + 스냅샷 저장 (DON-50)
//   "sessions"        — 활성 세션 목록 (Phase 3 확장)
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
//   policy_explain/policy_rollback/policy_reload 실패는 데이터패스로 전파하지 않는다.
//   롤백/리로드 실패 시 현재 정책을 유지한다 (fail-close).
// ---------------------------------------------------------------------------

#include <sys/types.h>

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/thread_pool.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "parser/sql_parser.hpp"
#include "policy/policy_engine.hpp"
#include "policy/policy_version_store.hpp"
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

    // 생성자 (policy_versions/policy_rollback/policy_reload 활성, DON-50)
    //   version_store      : 정책 버전 스토어 (nullptr 이면 버전 관리 커맨드 비활성)
    //   policy_config_path : 정책 파일 경로 (policy_reload 시 사용)
    UdsServer(const std::filesystem::path& socket_path,
              std::shared_ptr<StatsCollector> stats,
              std::shared_ptr<PolicyEngine> policy_engine,
              std::shared_ptr<SqlParser> sql_parser,
              std::shared_ptr<PolicyVersionStore> version_store,
              std::filesystem::path policy_config_path,
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

    // --- DON-53: UDS 보안 설정 setter ---
    void set_client_timeout(std::uint32_t timeout_sec);
    void set_max_connections(std::uint32_t max_conn);
    void set_allowed_uid(uid_t uid);

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

    // handle_policy_versions (DON-50)
    //   "policy_versions" 커맨드 처리.
    //   version_store_->list_versions() 와 policy_engine_->current_version() 을
    //   결합하여 버전 목록 JSON 을 반환한다.
    //   version_store_ 가 nullptr 이면 not-implemented 응답 반환.
    [[nodiscard]] std::string handle_policy_versions(std::string_view request_json);

    // handle_policy_rollback (DON-50)
    //   "policy_rollback" 커맨드 처리.
    //   payload 에서 target_version 을 파싱하고 version_store_->load_snapshot() 후
    //   policy_engine_->reload() 를 호출한다.
    //   실패 시 현재 정책 유지 (fail-close). version_store_ 가 nullptr 이면
    //   not-implemented 응답 반환.
    [[nodiscard]] std::string handle_policy_rollback(std::string_view request_json);

    // handle_policy_reload (DON-50, 기존 501 placeholder 교체)
    //   "policy_reload" 커맨드 처리.
    //   PolicyLoader::load(policy_config_path_) → save_snapshot() → reload() 순서로 수행.
    //   실패 시 현재 정책 유지 (fail-close). version_store_ 가 nullptr 이면
    //   not-implemented 응답 반환.
    [[nodiscard]] std::string handle_policy_reload(std::string_view request_json);

    std::filesystem::path socket_path_;
    std::shared_ptr<StatsCollector> stats_;
    std::shared_ptr<PolicyEngine> policy_engine_;        // nullable
    std::shared_ptr<SqlParser> sql_parser_;              // nullable
    std::shared_ptr<PolicyVersionStore> version_store_;  // nullable (DON-50)
    std::filesystem::path policy_config_path_;           // reload 시 사용할 정책 파일 경로 (DON-50)
    asio::io_context& ioc_;
    asio::local::stream_protocol::acceptor acceptor_;
    std::atomic<bool> stop_requested_{false};

    // --- DON-53: UDS 보안 멤버 ---
    std::atomic<std::uint32_t> client_timeout_sec_{30};  // 읽기 타임아웃 (초)
    std::atomic<std::uint32_t> max_connections_{8};      // 최대 동시 제어 연결 수
    std::atomic<uid_t> allowed_uid_{0};                  // 허용 UID (기본: 프로세스 자신)
    std::atomic<bool> allowed_uid_set_{false};           // set_allowed_uid() 호출 여부
    // handle_client 코루틴이 서버 객체 파괴 이후 정리될 수 있으므로
    // 연결 카운터 수명은 코루틴 프레임과 분리해 관리한다.
    std::shared_ptr<std::atomic<std::uint32_t>> active_connections_{
        std::make_shared<std::atomic<std::uint32_t>>(0)};

    // control_pool_:
    //   policy_reload/policy_rollback 의 동기 파일 I/O를 io_context 이벤트 루프에서 분리한다.
    //   단일 워커로 직렬 실행하여 관리 경로 경쟁을 최소화한다.
    asio::thread_pool control_pool_{1};
};
