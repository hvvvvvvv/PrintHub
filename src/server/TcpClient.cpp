#include "TcpClient.h"
#include <config.h>

#pragma warning(disable:4996)

using namespace std;

TcpClient::TcpClient(SOCKET socket, const sockaddr_in &addr)
    : m_socket(socket), m_addr(inet_ntoa(addr.sin_addr)), m_port(ntohs(addr.sin_port)) {
    setSocketTimeout(RECV_TIMEOUT);
}

TcpClient::~TcpClient() {
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
    }
}

void TcpClient::Write(const string &data) {
    int bytesSent = send(m_socket, data.c_str(), static_cast<int>(data.size()), 0);
    if (bytesSent == SOCKET_ERROR) {
        auto result = WSAGetLastError();
        if(result == WSAENOTCONN) {
            throw TcpClientDisconnected(m_addr, m_port);
        } else if (result == WSAETIMEDOUT) {
            throw TcpClientTimeout(m_addr, m_port);
        } else {
            throw TcpClientSendError(m_addr, m_port, result);
        }
    }
}

void TcpClient::Write(const char data) {
    return Write(string(1, data));
}

void TcpClient::Read(size_t size, BufferCallback callback) {
    if (size > MAX_RECV_SIZE) {
        throw invalid_argument("Requested size exceeds maximum limit");
    }
    if (!size) {
        throw invalid_argument("Invalid size requested");
    }

    auto recvLen = min(size, MAX_RECV_BUFFER_SIZE);
    size_t totalBytes = 0;
    auto buffer = string(MAX_RECV_BUFFER_SIZE, '\0');

    if(!leftoverBuffer.empty()) {
        size_t leftoverSize = min(leftoverBuffer.size(), size);
        if(callback) {
            callback(leftoverBuffer.data(), leftoverSize);
        }
        leftoverBuffer.erase(0, leftoverSize);
        totalBytes += leftoverSize;
        recvLen = min(size - totalBytes, MAX_RECV_BUFFER_SIZE);
    }

    while (totalBytes < size) {
        int bytesRead = recv(m_socket, buffer.data(), static_cast<int>(recvLen), 0);
        if (bytesRead > 0) {
            totalBytes += static_cast<size_t>(bytesRead);
            if(callback) {
                callback(buffer.data(), static_cast<size_t>(bytesRead));
            }
            recvLen = min(size - totalBytes, MAX_RECV_BUFFER_SIZE);
        } else if (bytesRead == 0) {
            throw TcpClientDisconnected(m_addr, m_port);
        } else {
            if(WSAGetLastError() == WSAETIMEDOUT) {
                throw TcpClientTimeout(m_addr, m_port);
            }
            if (WSAGetLastError() == WSAENOTCONN) {
                throw TcpClientDisconnected(m_addr, m_port);
            }
            throw TcpClientReceiveError(m_addr, m_port, WSAGetLastError());
        }
    }
}

void TcpClient::ReadAll(BufferCallback callback) {
    string buffer(MAX_RECV_BUFFER_SIZE, '\0');
    int bytesReceived;
    size_t totalBytes = 0;

    if (!leftoverBuffer.empty()) {
        if(callback) {
            callback(leftoverBuffer.data(), leftoverBuffer.size());
        }
        totalBytes += leftoverBuffer.size();
        leftoverBuffer.clear();
    }

    while ((bytesReceived = recv(m_socket, buffer.data(), MAX_RECV_BUFFER_SIZE, 0)) > 0) {
		totalBytes += static_cast<size_t>(bytesReceived);
        if (totalBytes > MAX_RECV_SIZE) {
            throw ::runtime_error("Total received data exceeds maximum limit");
        }
        if(callback) {
            callback(buffer.data(), static_cast<size_t>(bytesReceived));
        }
    }

    if (bytesReceived < 0) {
        throw ::system_error(WSAGetLastError(), ::system_category(), "Receive failed");
    }
}

void TcpClient::ReadLine(string &data) {
    auto it = leftoverBuffer.find('\n');
    if (it != ::string::npos) {
        data = leftoverBuffer.substr(0, it);
        leftoverBuffer.erase(0, it + 1);
        return;
    }
    
    auto buffer = ::string(MAX_RECV_BUFFER_SIZE, '\0');
    int bytesReceived;
    data = leftoverBuffer;
    leftoverBuffer.clear();

    while ((bytesReceived = recv(m_socket, buffer.data(), MAX_RECV_BUFFER_SIZE, 0)) > 0) {
        if (data.size() > MAX_RECV_SIZE) {
            throw ::runtime_error("Total received data exceeds maximum limit");
        }
        auto pos = buffer.find('\n', 0);
        if (pos != ::string::npos) {
            data.append(buffer.substr(0, pos));
            if (pos + 1 < bytesReceived) {
                leftoverBuffer = buffer.substr(pos + 1, bytesReceived - pos - 1);
            }
            return;
        }
        data.append(buffer.substr(0, bytesReceived));
        ::fill(buffer.begin(), buffer.end(), '\0'); // Clear buffer
    }
 
    if (bytesReceived < 0) {
        auto error = WSAGetLastError();
        if (error == WSAETIMEDOUT) {
            throw TcpClientTimeout(m_addr, m_port);
        } else if (error == WSAENOTCONN) {
            throw TcpClientDisconnected(m_addr, m_port);
        } else {
            throw TcpClientReceiveError(m_addr, m_port, error);
        }
    } else if (bytesReceived == 0 && data.empty()) {
        throw TcpClientDisconnected(m_addr, m_port);
    }
}


::string TcpClient::Addr() const {
    return m_addr;
}

uint16_t TcpClient::Port() const {
    return m_port;
}

void TcpClient::setSocketTimeout(DWORD timeout) {
    if (timeout <= 0) {
        throw ::invalid_argument("Socket timeout must be greater than 0");
    }
    if (setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR) {
        throw ::system_error(WSAGetLastError(), ::system_category(), "Failed to set socket receive timeout");
    }
    if (setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR) {
       throw ::system_error(WSAGetLastError(), ::system_category(), "Failed to set socket send timeout");
    }
}
