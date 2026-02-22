#include "proxy/session.hpp"

// ---------------------------------------------------------------------------
// Session — stub implementation
// ---------------------------------------------------------------------------

Session::Session(std::uint64_t                          session_id,
                 boost::asio::ip::tcp::socket           client_socket,
                 boost::asio::ip::tcp::endpoint         server_endpoint,
                 std::shared_ptr<PolicyEngine>          policy,
                 std::shared_ptr<StructuredLogger>      logger,
                 std::shared_ptr<StatsCollector>        stats)
    : session_id_{session_id}
    , client_socket_{std::move(client_socket)}
    , server_socket_{client_socket_.get_executor()}
    , server_endpoint_{std::move(server_endpoint)}
    , policy_{std::move(policy)}
    , logger_{std::move(logger)}
    , stats_{std::move(stats)}
    , ctx_{}
    , state_{SessionState::kHandshaking}
    , strand_{boost::asio::make_strand(client_socket_.get_executor())}
{}

auto Session::run() -> boost::asio::awaitable<void>
{
    co_return;
}

void Session::close()
{
    // stub
}

auto Session::state() const noexcept -> SessionState
{
    return state_;
}

auto Session::context() const noexcept -> const SessionContext&
{
    return ctx_;
}
