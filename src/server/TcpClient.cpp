#include "TcpClient.h"
#include <config.h>

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <utility>

#pragma warning(disable:4996)

TcpClient::TcpClient(UniqueSocket socket, const sockaddr_in& addr)
    : m_socket(std::move(socket)), m_addr(inet_ntoa(addr.sin_addr)), m_port(ntohs(addr.sin_port)) {
    setSocketTimeout(RECEIVE_TIMEOUT_MS);
}

TcpClient::~TcpClient() = default;

void TcpClient::Write(const std::string& data) {
    size_t totalBytesSent = 0;
    while (totalBytesSent < data.size()) {
        const auto remaining = data.size() - totalBytesSent;
        const auto chunkSize = static_cast<int>((std::min)(remaining, static_cast<size_t>(INT_MAX)));
        const int bytesSent = send(m_socket.get(), data.data() + totalBytesSent, chunkSize, 0);
        if (bytesSent > 0) {
            totalBytesSent += static_cast<size_t>(bytesSent);
            continue;
        }

        const auto result = bytesSent == 0 ? WSAECONNRESET : WSAGetLastError();
        if (result == WSAENOTCONN || result == WSAECONNRESET) {
            throw TcpClientDisconnected(m_addr, m_port);
        }
        if (result == WSAETIMEDOUT) {
            throw TcpClientTimeout(m_addr, m_port);
        }
        throw TcpClientSendError(m_addr, m_port, result);
    }
}

void TcpClient::Write(const char data) {
    Write(std::string(1, data));
}

void TcpClient::Read(size_t size, BufferCallback callback) {
    if (!size) {
        throw std::invalid_argument("Invalid size requested");
    }

    auto recvLen = (std::min)(size, RECEIVE_BUFFER_SIZE_BYTES);
    size_t totalBytes = 0;
    auto buffer = std::string(RECEIVE_BUFFER_SIZE_BYTES, '\0');

    if (!leftoverBuffer.empty()) {
        const size_t leftoverSize = (std::min)(leftoverBuffer.size(), size);
        if (callback) {
            callback(leftoverBuffer.data(), leftoverSize);
        }
        leftoverBuffer.erase(0, leftoverSize);
        totalBytes += leftoverSize;
        recvLen = (std::min)(size - totalBytes, RECEIVE_BUFFER_SIZE_BYTES);
    }

    while (totalBytes < size) {
        const int bytesRead = Receive(buffer.data(), static_cast<int>(recvLen));
        if (bytesRead > 0) {
            totalBytes += static_cast<size_t>(bytesRead);
            if (callback) {
                callback(buffer.data(), static_cast<size_t>(bytesRead));
            }
            recvLen = (std::min)(size - totalBytes, RECEIVE_BUFFER_SIZE_BYTES);
            continue;
        }
        if (bytesRead == 0) {
            throw TcpClientDisconnected(m_addr, m_port);
        }
    }
}

void TcpClient::ReadAll(BufferCallback callback) {
    std::string buffer(RECEIVE_BUFFER_SIZE_BYTES, '\0');

    if (!leftoverBuffer.empty()) {
        if (callback) {
            callback(leftoverBuffer.data(), leftoverBuffer.size());
        }
        leftoverBuffer.clear();
    }

    while (true) {
        const int bytesReceived = Receive(buffer.data(), static_cast<int>(buffer.size()));
        if (bytesReceived > 0) {
            if (callback) {
                callback(buffer.data(), static_cast<size_t>(bytesReceived));
            }
            continue;
        }
        if (bytesReceived == 0) {
            return;
        }
    }
}

void TcpClient::ReadLine(std::string& data) {
    const auto newline = leftoverBuffer.find('\n');
    if (newline != std::string::npos) {
        data = leftoverBuffer.substr(0, newline);
        leftoverBuffer.erase(0, newline + 1);
        return;
    }

    auto buffer = std::string(RECEIVE_BUFFER_SIZE_BYTES, '\0');
    data = std::move(leftoverBuffer);
    leftoverBuffer.clear();

    while (true) {
        const int bytesReceived = Receive(buffer.data(), static_cast<int>(buffer.size()));
        if (bytesReceived > 0) {
            const auto receivedSize = static_cast<size_t>(bytesReceived);
            const auto pos = buffer.find('\n', 0);
            if (pos != std::string::npos && pos < receivedSize) {
                data.append(buffer.data(), pos);
                if (data.size() > MAX_COMMAND_SIZE_BYTES) {
                    throw TcpClientInvalidData(m_addr, m_port);
                }
                if (pos + 1 < receivedSize) {
                    leftoverBuffer.assign(buffer.data() + pos + 1, receivedSize - pos - 1);
                }
                return;
            }

            data.append(buffer.data(), receivedSize);
            if (data.size() > MAX_COMMAND_SIZE_BYTES) {
                throw TcpClientInvalidData(m_addr, m_port);
            }
            continue;
        }
        if (bytesReceived == 0) {
            throw TcpClientDisconnected(m_addr, m_port);
        }
    }
}

std::string TcpClient::Addr() const {
    return m_addr;
}

uint16_t TcpClient::Port() const {
    return m_port;
}

int TcpClient::Receive(char* buffer, int size) {
    const int bytesReceived = recv(m_socket.get(), buffer, size, 0);
    if (bytesReceived >= 0) {
        return bytesReceived;
    }

    const auto error = WSAGetLastError();
    if (error == WSAETIMEDOUT) {
        throw TcpClientTimeout(m_addr, m_port);
    }
    if (error == WSAENOTCONN || error == WSAECONNRESET) {
        throw TcpClientDisconnected(m_addr, m_port);
    }
    throw TcpClientReceiveError(m_addr, m_port, error);
}

void TcpClient::setSocketTimeout(DWORD timeout) {
    if (timeout == 0) {
        throw std::invalid_argument("Socket timeout must be greater than 0");
    }
    if (setsockopt(m_socket.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR) {
        throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to set socket receive timeout");
    }
    if (setsockopt(m_socket.get(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR) {
        throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to set socket send timeout");
    }
}
