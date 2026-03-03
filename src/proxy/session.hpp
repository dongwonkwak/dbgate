#pragma once

#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/strand.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/async_stream.hpp"
#include "common/types.hpp"
#include "logger/structured_logger.hpp"
#include "parser/injection_detector.hpp"
#include "parser/procedure_detector.hpp"
#include "parser/sql_parser.hpp"
#include "policy/policy_engine.hpp"
#include "protocol/command.hpp"
#include "protocol/handshake.hpp"
#include "protocol/mysql_packet.hpp"
#include "stats/stats_collector.hpp"

// ---------------------------------------------------------------------------
// SessionState
//   Session 의 생명주기 상태를 나타낸다.
//
//   kHandshaking     : MySQL 핸드셰이크 진행 중
//   kReady           : 핸드셰이크 완료, 커맨드 수신 대기 중
//   kProcessingQuery : COM_QUERY 처리 중 (정책 판정 / 릴레이 진행 중)
//   kClosing         : Graceful Shutdown 진행 중 (새 쿼리 거부)
//   kClosed          : 세션 완전 종료
// ---------------------------------------------------------------------------
enum class SessionState : std::uint8_t {
    kHandshaking = 0,
    kReady = 1,
    kProcessingQuery = 2,
    kClosing = 3,
    kClosed = 4,
};

// ---------------------------------------------------------------------------
// Session
//   클라이언트 1개와 MySQL 서버 1개를 1:1 로 릴레이하는 세션.
//
//   생명주기:
//     1. 생성자에서 소켓/의존성 주입
//     2. run() 코루틴으로 핸드셰이크 → 커맨드 루프 수행
//     3. close() 또는 EOF/에러 감지 시 kClosing → kClosed 전이
//
//   스레드 안전성:
//     모든 비동기 핸들러는 strand_ 위에서 직렬화되므로 수동 락이 불필요하다.
//
//   SSL 지원:
//     - client_stream: ProxyServer에서 accept 시 TCP 또는 TLS AsyncStream으로
//       이미 결정되어 생성자에 전달됨.
//     - server_stream: run() 내부에서 TCP connect 후 backend_ssl_ctx_가
//       유효하면 TLS 업그레이드하여 AsyncStream으로 재생성.
// ---------------------------------------------------------------------------
class Session : public std::enable_shared_from_this<Session> {
public:
    // -----------------------------------------------------------------------
    // 생성자
    //   session_id       : 프로세스 범위 유일 ID
    //   client_stream    : accept 된 클라이언트 AsyncStream (move 소유권 이전)
    //                      ProxyServer가 frontend SSL 여부에 따라 생성하여 전달
    //   server_endpoint  : 업스트림 MySQL 서버 엔드포인트
    //   backend_ssl_ctx  : Backend TLS용 ssl::context 포인터
    //                      nullptr이면 backend 평문 모드
    //                      유효한 포인터면 backend TLS 모드
    //   policy           : 정책 판정 엔진 (shared 소유권)
    //   logger           : 구조화 로거 (shared 소유권)
    //   stats            : 통계 수집기 (shared 소유권)
    // -----------------------------------------------------------------------
    Session(std::uint64_t session_id,
            AsyncStream client_stream,
            boost::asio::ip::tcp::endpoint server_endpoint,
            boost::asio::ssl::context* backend_ssl_ctx,
            bool backend_ssl_verify,
            const std::string& backend_tls_server_name,
            std::shared_ptr<PolicyEngine> policy,
            std::shared_ptr<StructuredLogger> logger,
            std::shared_ptr<StatsCollector> stats);

    ~Session() = default;

    // 복사/이동 금지 (shared_ptr 로만 관리)
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // -----------------------------------------------------------------------
    // run
    //   세션의 메인 코루틴.
    //   핸드셰이크 → 커맨드 루프 → 정리 순으로 동작한다.
    //   반환 시 세션은 kClosed 상태가 된다.
    // -----------------------------------------------------------------------
    auto run() -> boost::asio::awaitable<void>;

    // 세션 코루틴/종료 작업을 직렬화할 executor(strand) 반환.
    [[nodiscard]] auto executor() const -> boost::asio::any_io_executor { return strand_; }

    // -----------------------------------------------------------------------
    // close
    //   세션을 정상 종료한다. 현재 처리 중인 쿼리가 완료된 후 종료된다.
    //   이미 kClosing / kClosed 상태이면 no-op.
    // -----------------------------------------------------------------------
    void close();

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------
    [[nodiscard]] auto state() const noexcept -> SessionState;
    [[nodiscard]] auto context() const noexcept -> const SessionContext&;

private:
    std::uint64_t session_id_;
    AsyncStream client_stream_;
    // server_stream_은 run()에서 TCP connect 후 생성 (backend SSL 판단 포함)
    // 임시 tcp::socket으로 초기화, run()에서 AsyncStream으로 move-assign
    AsyncStream server_stream_;
    boost::asio::ip::tcp::endpoint server_endpoint_;

    // Backend TLS: nullptr이면 평문 모드, 유효하면 TLS 모드
    boost::asio::ssl::context* backend_ssl_ctx_{nullptr};
    bool backend_ssl_verify_{false};
    std::string backend_tls_server_name_{};

    std::shared_ptr<PolicyEngine> policy_;
    std::shared_ptr<StructuredLogger> logger_;
    std::shared_ptr<StatsCollector> stats_;

    SessionContext ctx_;
    SessionState state_{SessionState::kHandshaking};

    // strand: 모든 비동기 핸들러를 직렬화하여 스레드 안전성을 보장한다.
    boost::asio::strand<boost::asio::any_io_executor> strand_;

    // parser 멤버 (stateless이므로 재사용)
    SqlParser sql_parser_{};
    InjectionDetector injection_detector_;
    ProcedureDetector proc_detector_{};

    // close() 중복 호출 방지용 atomic 플래그
    std::atomic<bool> closing_{false};

    // relay_server_response 헬퍼
    //   MySQL 서버 응답(Result Set / OK / ERR)이 완료될 때까지 읽어 클라이언트에 릴레이.
    auto relay_server_response(CommandType request_type, std::uint8_t request_seq_id)
        -> boost::asio::awaitable<std::expected<void, ParseError>>;
};
