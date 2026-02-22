#include "proxy/proxy_server.hpp"

// ---------------------------------------------------------------------------
// ProxyServer — stub implementation
// ---------------------------------------------------------------------------

ProxyServer::ProxyServer(ProxyConfig config)
    : config_{std::move(config)}
{}

void ProxyServer::run(boost::asio::io_context& /*io_ctx*/)
{
    // stub
}

void ProxyServer::stop()
{
    // stub
}
