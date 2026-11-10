#include <Server.h>
#include <spdlog/spdlog.h>

#pragma warning(disable:4996)

namespace {

constexpr int kSocketOptionEnabled = 1;
constexpr int kInetPtonSuccess = 1;

} // namespace

PrintServer::PrintServer(uint16_t port, int minThreads, int maxPendingClients) : m_taskQueue(static_cast<size_t>(maxPendingClients)) {
    m_listenAddr = {};
    m_listenAddr.sin_family = AF_INET;
    m_listenAddr.sin_addr.s_addr = INADDR_ANY;
    SetListenPort(port);
    SetMinThreads(minThreads);
    if (maxPendingClients < 1) {
        throw std::invalid_argument("Maximum pending clients must be at least 1");
    }
    m_shutdown = true;
}

PrintServer::~PrintServer() {
    Stop();
}

void PrintServer::SetListenAddr(const std::string& addr) {
    if (inet_pton(AF_INET, addr.c_str(), &m_listenAddr.sin_addr) != kInetPtonSuccess) {
        throw std::invalid_argument("Invalid IPv4 listen address");
    }
}

void PrintServer::SetListenPort(uint16_t port) {
    if (port == 0) {
        throw std::invalid_argument("Port number cannot be zero");
    }
    m_listenAddr.sin_port = htons(port);
}

void PrintServer::SetMinThreads(int minThreads) {
    if (minThreads < 1) {
        throw std::invalid_argument("Worker thread count must be at least 1");
    }
    m_minThreads = minThreads;
}

std::string PrintServer::GetListenAddr() const {
    char addr[INET_ADDRSTRLEN]{};
    return inet_ntop(AF_INET, &m_listenAddr.sin_addr, addr, sizeof(addr)) ? addr : "<invalid>";
}

uint16_t PrintServer::GetListenPort() const {
    return ntohs(m_listenAddr.sin_port);
}

void PrintServer::Start() {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    if (!m_shutdown) {
        return;
    }

    m_listenSocket.reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (m_listenSocket.get() == INVALID_SOCKET) {
        throw std::system_error(WSAGetLastError(), std::system_category(), "Socket creation failed");
    }

    const int reuseAddress = kSocketOptionEnabled;
    const auto failStart = [this](const char* message) {
        const auto error = WSAGetLastError();
        m_listenSocket.reset();
        throw std::system_error(error, std::system_category(), message);
    };
    if (setsockopt(m_listenSocket.get(), SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress)) == SOCKET_ERROR) {
        failStart("setsockopt failed");
    }
    if (bind(m_listenSocket.get(), reinterpret_cast<sockaddr*>(&m_listenAddr), sizeof(m_listenAddr)) == SOCKET_ERROR) {
        failStart("Bind failed");
    }
    if (listen(m_listenSocket.get(), SOMAXCONN) == SOCKET_ERROR) {
        failStart("Listen failed");
    }

    m_workerThreads.clear();
    m_stopping = false;
    m_shutdown = false;
    try {
        for (int i = 0; i < m_minThreads; ++i) {
            m_workerThreads.emplace_back([this](std::stop_token stopToken) { workerThread(stopToken); });
        }
        const auto listenSocket = m_listenSocket.get();
        m_acceptThread = std::jthread([this, listenSocket](std::stop_token stopToken) { listenLoop(stopToken, listenSocket); });
    } catch (...) {
        // Workers already launched must be joined while their queue and locks still exist.
        stopLocked();
        throw;
    }
}

void PrintServer::Stop() {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    stopLocked();
}

void PrintServer::stopLocked() {
    if (m_shutdown) {
        return;
    }

    m_stopping = true;
    m_listenSocket.reset();
    m_acceptThread.request_stop();
    {
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        m_taskQueue.Clear();
    }
    m_condition.notify_all();
    {
        std::lock_guard<std::mutex> activeLock(m_activeSocketsMutex);
        for (const auto socket : m_activeSockets) {
            shutdown(socket, SD_BOTH);
        }
    }

    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }
    for (auto& thread : m_workerThreads) {
        thread.request_stop();
    }
    for (auto& thread : m_workerThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    m_workerThreads.clear();

    m_shutdown = true;
}

void PrintServer::listenLoop(std::stop_token stopToken, SOCKET listenSocket) {
    while (!stopToken.stop_requested()) {
        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);
        UniqueSocket clientSocket(accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen));
        if (clientSocket.get() == INVALID_SOCKET) {
            const auto error = WSAGetLastError();
            if (stopToken.stop_requested() || error == WSAEINTR || error == WSAENOTSOCK || error == WSAEINVAL) {
                return;
            }
            spdlog::warn("accept failed with WSA error {}", error);
            continue;
        }
        bool accepted = false;
        {
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            if (!m_stopping && m_taskQueue.TryPush(PendingClient{ std::move(clientSocket), clientAddr })) {
                accepted = true;
            }
        }
        if (!accepted) {
            ++m_rejectedPendingClients;
            const auto now = std::chrono::steady_clock::now();
            if (m_lastQueueRejectionLog.time_since_epoch().count() == 0 ||
                now - m_lastQueueRejectionLog >= std::chrono::minutes(1)) {
                spdlog::warn("Rejected {} pending clients on {}:{} because the queue is full or stopping",
                    m_rejectedPendingClients, GetListenAddr(), GetListenPort());
                m_rejectedPendingClients = 0;
                m_lastQueueRejectionLog = now;
            }
            continue;
        }
        m_condition.notify_one();
    }
}

void PrintServer::workerThread(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        PendingClient pendingClient;
        {
            std::unique_lock<std::mutex> queueLock(m_queueMutex);
            const auto hasData = m_condition.wait(queueLock, stopToken, [this] { return m_stopping || !m_taskQueue.Empty(); });
            if (!hasData || m_stopping || stopToken.stop_requested()) {
                return;
            }
            pendingClient = m_taskQueue.Pop();
        }

        const auto clientSocket = pendingClient.socket.get();
        const auto clientAddr = pendingClient.address;
        std::optional<TcpClient> client;
        bool active = false;
        try {
            {
                std::lock_guard<std::mutex> activeLock(m_activeSocketsMutex);
                if (m_stopping || stopToken.stop_requested()) return;
                m_activeSockets.insert(clientSocket);
                active = true;
            }
            client.emplace(std::move(pendingClient.socket), clientAddr);
            handleClient(*client);
        }
        catch (const std::exception& e) {
            char addr[INET_ADDRSTRLEN]{};
            spdlog::warn("Client {}:{} failed: {}", inet_ntop(AF_INET, &clientAddr.sin_addr, addr, sizeof(addr)), ntohs(clientAddr.sin_port), e.what());
        }
        catch (...) {
            spdlog::warn("Client worker failed with an unknown error");
        }
        if (active) {
            std::lock_guard<std::mutex> activeLock(m_activeSocketsMutex);
            m_activeSockets.erase(clientSocket);
        }
    }
}
