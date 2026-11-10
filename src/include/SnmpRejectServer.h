#pragma once

#include <SnmpServer.h>

class SnmpRejectServer {
public:
    static constexpr uint16_t PORT = SnmpServer::PORT;

    explicit SnmpRejectServer(std::string listenAddress);
    ~SnmpRejectServer();

    SnmpRejectServer(const SnmpRejectServer&) = delete;
    SnmpRejectServer& operator=(const SnmpRejectServer&) = delete;

    void Start();
    void Stop();

private:
    SnmpServer m_server;
};
