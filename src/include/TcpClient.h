#include <winsock2.h>
#include <SocketHandle.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <stdexcept>
#include <string>


class TcpClient {
    public:
        using BufferCallback = std::function<void(const char *data, size_t size)>;
        TcpClient(UniqueSocket socket, const sockaddr_in &addr);
        ~TcpClient();
        TcpClient(const TcpClient&) = delete;
        TcpClient& operator=(const TcpClient&) = delete;
        TcpClient(TcpClient&&) noexcept = default;
        TcpClient& operator=(TcpClient&&) noexcept = default;
        void Write(const std::string &data);
        void Write(const char data);
        void Read(size_t size, BufferCallback callback);
        void ReadAll(BufferCallback callback);
        void ReadLine(std::string &data);
        std::string Addr() const;
        uint16_t Port() const;
    private:
        UniqueSocket m_socket;
        std::string leftoverBuffer;
        std::string m_addr;
        uint16_t m_port;
        int Receive(char* buffer, int size);
        void setSocketTimeout(DWORD timeout);
};

class TcpClientError : public std::runtime_error {
    public:
        TcpClientError(const std::string &message, const std::string &addr, uint16_t port)
            : std::runtime_error(fmt::format("TcpClient error: {} from {}:{}", message, addr, std::to_string(port))) {}
};

class TcpClientDisconnected : public TcpClientError {
    public:
        TcpClientDisconnected(const std::string &addr, uint16_t port)
            : TcpClientError("TcpClient disconnected: ", addr, port) {}
};

class TcpClientTimeout : public TcpClientError {
    public:
        TcpClientTimeout(const std::string &addr, uint16_t port)
            : TcpClientError("TcpClient timeout: ", addr, port) {}
};

class TcpClientInvalidData : public TcpClientError {
    public:
        TcpClientInvalidData(const std::string &addr, uint16_t port)
            : TcpClientError("TcpClient received invalid data: ", addr, port) {}
};

class TcpClientReceiveError : public TcpClientError {
    public:
        TcpClientReceiveError(const std::string &addr, uint16_t port, int wsaError)
            : TcpClientError(fmt::format("TcpClient receive failed, WSA Error code: {}", std::to_string(wsaError)), addr, port) {}
};

class TcpClientSendError : public TcpClientError {
    public:
        TcpClientSendError(const std::string &addr, uint16_t port, int wsaError)
            : TcpClientError(fmt::format("TcpClient send failed, WSA Error code: {}", std::to_string(wsaError)), addr, port) {}
};
