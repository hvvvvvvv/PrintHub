#include <SnmpRejectServer.h>

#include <SnmpRejectSession.h>

#include <array>
#include <stdexcept>
#include <system_error>

#include <spdlog/spdlog.h>
#include <ws2tcpip.h>

SnmpRejectServer::SnmpRejectServer(std::string listenAddress)
    : m_listenAddress(std::move(listenAddress)) {
}

SnmpRejectServer::~SnmpRejectServer() {
    Stop();
}

void SnmpRejectServer::Start() {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    if (m_socket.get() != INVALID_SOCKET) {
        return;
    }

    UniqueSocket socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (socket.get() == INVALID_SOCKET) {
        throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to create SNMP UDP socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    if (inet_pton(AF_INET, m_listenAddress.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("Invalid IPv4 listen address for SNMP reject server");
    }
    if (bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to bind SNMP reject server to UDP port 161");
    }

    const auto nativeSocket = socket.get();
    m_socket = std::move(socket);
    m_receiveThread = std::jthread([this, nativeSocket](std::stop_token stopToken) { receiveLoop(stopToken, nativeSocket); });
}

void SnmpRejectServer::Stop() {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    if (m_socket.get() == INVALID_SOCKET) {
        return;
    }

    m_socket.reset();
    m_receiveThread.request_stop();
    if (m_receiveThread.joinable()) {
        m_receiveThread.join();
    }
}

void SnmpRejectServer::receiveLoop(std::stop_token stopToken, SOCKET socket) {
    std::array<char, SnmpRejectSession::MAX_MESSAGE_SIZE_BYTES> buffer{};
    while (!stopToken.stop_requested()) {
        sockaddr_in clientAddress{};
        int clientAddressSize = sizeof(clientAddress);
        const auto bytesReceived = recvfrom(socket, buffer.data(), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&clientAddress), &clientAddressSize);
        if (bytesReceived == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            if (stopToken.stop_requested() || error == WSAENOTSOCK || error == WSAESHUTDOWN) {
                return;
            }
            if (error != WSAEMSGSIZE) {
                spdlog::warn("SNMP reject server receive failed with WSA error {}", error);
            }
            continue;
        }

        const auto response = SnmpRejectSession::Handle(std::string_view(buffer.data(), static_cast<size_t>(bytesReceived)));
        if (!response.has_value()) {
            continue;
        }
        const auto bytesSent = sendto(socket, response->data(), static_cast<int>(response->size()), 0,
            reinterpret_cast<const sockaddr*>(&clientAddress), clientAddressSize);
        if (bytesSent == SOCKET_ERROR) {
            spdlog::warn("SNMP reject server send failed with WSA error {}", WSAGetLastError());
            continue;
        }
        spdlog::debug("Rejected SNMP discovery request from UDP client");
    }
}
