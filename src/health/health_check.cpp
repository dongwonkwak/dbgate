#include "health/health_check.hpp"

// ---------------------------------------------------------------------------
// HealthCheck — stub implementation
// ---------------------------------------------------------------------------

HealthCheck::HealthCheck(std::uint16_t                   port,
                         std::shared_ptr<StatsCollector> stats,
                         boost::asio::io_context&        io_context)
    : port_{port}
    , stats_{std::move(stats)}
    , io_context_{io_context}
{}

auto HealthCheck::run() -> boost::asio::awaitable<void>
{
    co_return;
}

void HealthCheck::set_unhealthy(std::string_view reason)
{
    unhealthy_reason_ = std::string{reason};
    status_ = HealthStatus::kUnhealthy;
}

void HealthCheck::set_healthy()
{
    unhealthy_reason_.clear();
    status_ = HealthStatus::kHealthy;
}

auto HealthCheck::status() const noexcept -> HealthStatus
{
    return status_;
}
