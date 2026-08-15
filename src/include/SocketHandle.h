#pragma once

#include <utility>

#include <winsock2.h>

class UniqueSocket {
public:
    UniqueSocket() = default;
    explicit UniqueSocket(SOCKET socket) noexcept : m_socket(socket) {}
    ~UniqueSocket() { reset(); }
    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;
    UniqueSocket(UniqueSocket&& other) noexcept : m_socket(std::exchange(other.m_socket, INVALID_SOCKET)) {}
    UniqueSocket& operator=(UniqueSocket&& other) noexcept {
        if (this != &other) {
            reset();
            m_socket = std::exchange(other.m_socket, INVALID_SOCKET);
        }
        return *this;
    }
    SOCKET get() const noexcept { return m_socket; }
    SOCKET release() noexcept { return std::exchange(m_socket, INVALID_SOCKET); }
    void reset(SOCKET socket = INVALID_SOCKET) noexcept {
        if (m_socket != INVALID_SOCKET) {
            closesocket(m_socket);
        }
        m_socket = socket;
    }

private:
    SOCKET m_socket = INVALID_SOCKET;
};
