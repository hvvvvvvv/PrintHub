#pragma once

#include <SocketHandle.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

class SnmpRejectServer {
public:
    static constexpr uint16_t PORT = 161;

    explicit SnmpRejectServer(std::string listenAddress);
    ~SnmpRejectServer();

    SnmpRejectServer(const SnmpRejectServer&) = delete;
    SnmpRejectServer& operator=(const SnmpRejectServer&) = delete;

    void Start();
    void Stop();

private:
    void receiveLoop(std::stop_token stopToken, SOCKET socket);

    std::string m_listenAddress;
    UniqueSocket m_socket;
    std::jthread m_receiveThread;
    std::mutex m_statusMutex;
};
