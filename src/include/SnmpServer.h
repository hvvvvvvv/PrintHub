#pragma once

#include <SocketHandle.h>

#include <cstdint>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

// UDP transport shared by the PPM agent and the legacy rejection responder.
class SnmpServer {
public:
    using RequestHandler = std::function<std::optional<std::string>(std::string_view)>;
    using FailureHandler = std::function<void(int)>;
    static constexpr uint16_t PORT = 161;

    explicit SnmpServer(std::string listenAddress, RequestHandler handler, uint16_t port = PORT,
        FailureHandler onFailure = {});
    ~SnmpServer();
    SnmpServer(const SnmpServer&) = delete;
    SnmpServer& operator=(const SnmpServer&) = delete;

    void Start();
    void Stop();
    int GetFailureCode() const noexcept { return m_failureCode.load(); }

private:
    void receiveLoop(std::stop_token stopToken, SOCKET socket);

    std::string m_listenAddress;
    RequestHandler m_handler;
    FailureHandler m_onFailure;
    uint16_t m_port;
    std::atomic<int> m_failureCode = 0;
    UniqueSocket m_socket;
    std::jthread m_receiveThread;
    std::mutex m_statusMutex;
};
