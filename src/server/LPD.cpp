#include <Server.h>

#include <LpdSession.h>
#include <PrinterJob.h>

namespace {

class TcpLpdTransport final : public ILpdTransport {
public:
    explicit TcpLpdTransport(TcpClient& client) : m_client(client) {}

    LpdEndpoint Endpoint() const override {
        return { m_client.Addr(), m_client.Port() };
    }

    void WriteByte(char value) override {
        m_client.Write(value);
    }

    void ReadLine(std::string& line) override {
        try {
            m_client.ReadLine(line);
        }
        catch (const TcpClientDisconnected&) {
            throw LpdTransportDisconnected(Endpoint());
        }
    }

    void ReadExactly(size_t size, const BufferCallback& callback) override {
        try {
            m_client.Read(size, callback);
        }
        catch (const TcpClientDisconnected&) {
            throw LpdTransportDisconnected(Endpoint());
        }
    }

    void ReadToEnd(const BufferCallback& callback) override {
        m_client.ReadAll(callback);
    }

private:
    TcpClient& m_client;
};

} // namespace

LPD::LPD(uint16_t port, std::vector<LpdQueueMapping> queueMappings, int minThreads)
    : PrintServer(port, minThreads), m_queueMappings(std::move(queueMappings)) {
}

void LPD::handleClient(TcpClient& client) {
    WinspoolPrinterJobFactory jobFactory;
    LpdSession session(jobFactory, [this](const std::string& queueName) {
        return ResolveLpdQueuePrinter(m_queueMappings, queueName);
    });
    TcpLpdTransport transport(client);
    session.Run(transport);
}
