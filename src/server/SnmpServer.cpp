#include <SnmpServer.h>

#include <SnmpProtocol.h>

#include <array>
#include <stdexcept>
#include <system_error>

#include <spdlog/spdlog.h>
#include <ws2tcpip.h>

namespace {
constexpr DWORD kReceiveTimeoutMs = 200;
constexpr DWORD kSendTimeoutMs = 500;
}

SnmpServer::SnmpServer(std::string listenAddress, RequestHandler handler, uint16_t port, FailureHandler onFailure)
    : m_listenAddress(std::move(listenAddress)), m_handler(std::move(handler)),
      m_onFailure(std::move(onFailure)), m_port(port) {
    if (!m_handler) throw std::invalid_argument("SNMP request handler is required");
}

SnmpServer::~SnmpServer() { Stop(); }

void SnmpServer::Start() {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    if (m_socket.get() != INVALID_SOCKET) return;

    UniqueSocket socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (socket.get() == INVALID_SOCKET) {
        throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to create SNMP UDP socket");
    }

    const int exclusive = 1;
    if (setsockopt(socket.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR ||
        setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&kReceiveTimeoutMs), sizeof(kReceiveTimeoutMs)) == SOCKET_ERROR ||
        setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&kSendTimeoutMs), sizeof(kSendTimeoutMs)) == SOCKET_ERROR) {
        throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to configure SNMP UDP socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(m_port);
    if (inet_pton(AF_INET, m_listenAddress.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("Invalid IPv4 listen address for SNMP server");
    }
    if (bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        throw std::system_error(WSAGetLastError(), std::system_category(),
            "Failed to bind SNMP server to " + m_listenAddress + ":" + std::to_string(m_port));
    }
    int addressSize = sizeof(address);
    if (getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &addressSize) == SOCKET_ERROR) {
        throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to read SNMP listen port");
    }

    const auto nativeSocket = socket.get();
    // Stop keeps the native handle alive until the receiver exits.
    m_failureCode.store(0);
    std::jthread receiver([this, nativeSocket](std::stop_token token) { receiveLoop(token, nativeSocket); });
    m_boundPort = ntohs(address.sin_port);
    m_socket = std::move(socket);
    m_receiveThread = std::move(receiver);
}

void SnmpServer::Stop() {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    if (m_socket.get() == INVALID_SOCKET) return;
    m_receiveThread.request_stop();
    if (m_receiveThread.joinable()) m_receiveThread.join();
    m_socket.reset();
    m_boundPort = 0;
}

void SnmpServer::receiveLoop(std::stop_token token, SOCKET socket) {
    std::array<char, snmp::kMaxMessageBytes> buffer{};
    while (!token.stop_requested()) {
        sockaddr_in peer{};
        int peerSize = sizeof(peer);
        const int received = recvfrom(socket, buffer.data(), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&peer), &peerSize);
        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (token.stop_requested()) return;
            if (error == WSAETIMEDOUT || error == WSAEMSGSIZE || error == WSAECONNRESET) continue;
            m_failureCode.store(error);
            spdlog::error("SNMP receive failed with WSA error {}", error);
            if (m_onFailure) m_onFailure(error);
            return;
        }
        if (token.stop_requested()) return;

        try {
            const auto response = m_handler(std::string_view(buffer.data(), static_cast<size_t>(received)));
            if (!response || response->size() > snmp::kMaxMessageBytes || token.stop_requested()) continue;
            const int sent = sendto(socket, response->data(), static_cast<int>(response->size()), 0,
                reinterpret_cast<const sockaddr*>(&peer), peerSize);
            if (sent == SOCKET_ERROR) {
                if (!token.stop_requested()) spdlog::debug("SNMP send failed with WSA error {}", WSAGetLastError());
                continue;
            }
            spdlog::debug("Answered SNMP discovery request");
        } catch (const std::exception& error) {
            spdlog::warn("SNMP request processing failed: {}", error.what());
        }
    }
}
