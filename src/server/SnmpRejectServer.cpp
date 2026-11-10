#include <SnmpRejectServer.h>
#include <SnmpRejectSession.h>

SnmpRejectServer::SnmpRejectServer(std::string listenAddress)
    : m_server(std::move(listenAddress), SnmpRejectSession::Handle) {}

SnmpRejectServer::~SnmpRejectServer() = default;

void SnmpRejectServer::Start() { m_server.Start(); }
void SnmpRejectServer::Stop() { m_server.Stop(); }
