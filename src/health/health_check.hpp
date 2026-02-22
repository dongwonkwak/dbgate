#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class StatsCollector;

// ---------------------------------------------------------------------------
// HealthStatus
//   Health Check 엔드포인트의 현재 상태를 나타낸다.
//
//   kHealthy   : 정상 동작 중 (HTTP 200 반환)
//   kUnhealthy : 비정상 상태 (HTTP 503 반환)
// ---------------------------------------------------------------------------
enum class HealthStatus : std::uint8_t {
    kHealthy   = 0,
    kUnhealthy = 1,
};

// ---------------------------------------------------------------------------
// HealthCheck
//   간단한 HTTP/1.1 Health Check 서버.
//
//   GET /health 요청에 대해 현재 HealthStatus 에 따라 응답한다.
//   - kHealthy   -> 200 OK  + JSON body {"status":"ok"}
//   - kUnhealthy -> 503 Service Unavailable + JSON body {"status":"unhealthy","reason":"..."}
//
//   과부하 / 업스트림 연결 실패 등의 상황에서 set_unhealthy() 를 호출하면
//   로드밸런서가 해당 인스턴스를 라우팅 대상에서 제외할 수 있다.
// ---------------------------------------------------------------------------
class HealthCheck {
public:
    // -----------------------------------------------------------------------
    // 생성자
    //   port       : Health Check HTTP 서버 리슨 포트 (config 에서 주입)
    //   stats      : 통계 수집기 (shared 소유권, 상태 응답에 포함 가능)
    //   io_context : Boost.Asio io_context (코루틴 실행에 사용)
    // -----------------------------------------------------------------------
    HealthCheck(std::uint16_t                   port,
                std::shared_ptr<StatsCollector> stats,
                boost::asio::io_context&        io_context);

    ~HealthCheck() = default;

    // 복사/이동 금지
    HealthCheck(const HealthCheck&)            = delete;
    HealthCheck& operator=(const HealthCheck&) = delete;
    HealthCheck(HealthCheck&&)                 = delete;
    HealthCheck& operator=(HealthCheck&&)      = delete;

    // -----------------------------------------------------------------------
    // run
    //   포트를 바인딩하고 Accept 루프를 시작한다.
    //   ProxyServer 의 io_context 에서 co_spawn 하여 실행한다.
    // -----------------------------------------------------------------------
    auto run() -> boost::asio::awaitable<void>;

    // -----------------------------------------------------------------------
    // set_unhealthy
    //   상태를 kUnhealthy 로 전환한다.
    //   reason : HTTP 응답 body 에 포함할 사유 문자열
    // -----------------------------------------------------------------------
    void set_unhealthy(std::string_view reason);

    // -----------------------------------------------------------------------
    // set_healthy
    //   상태를 kHealthy 로 복구한다.
    // -----------------------------------------------------------------------
    void set_healthy();

    // -----------------------------------------------------------------------
    // status
    //   현재 HealthStatus 를 반환한다. 스레드 안전하게 읽힐 수 있도록
    //   구현체에서 atomic 또는 strand 를 사용해야 한다.
    // -----------------------------------------------------------------------
    [[nodiscard]] auto status() const noexcept -> HealthStatus;

private:
    std::uint16_t                   port_;
    std::shared_ptr<StatsCollector> stats_;
    boost::asio::io_context&        io_context_;
    HealthStatus                    status_{HealthStatus::kHealthy};
    std::string                     unhealthy_reason_{};
};
