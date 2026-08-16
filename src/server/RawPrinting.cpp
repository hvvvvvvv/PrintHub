#include <Server.h>

#include <PrinterJob.h>
#include <RawPrintSession.h>
#include <spdlog/spdlog.h>

namespace {

class TcpRawPrintTransport final : public IRawPrintTransport {
public:
    explicit TcpRawPrintTransport(TcpClient& client) : m_client(client) {}

    void ReadToEnd(const BufferCallback& callback) override {
        m_client.ReadAll(callback);
    }

private:
    TcpClient& m_client;
};

} // namespace

RawPrinting::RawPrinting(std::string printerName, uint16_t port, int minThreads, int maxPendingClients)
    : PrintServer(port, minThreads, maxPendingClients), m_printerName(std::move(printerName)) {
    if (m_printerName.empty()) {
        throw std::invalid_argument("Printer name cannot be empty");
    }
}

void RawPrinting::handleClient(TcpClient& client) {
    try {
        spdlog::info("Received RAW print job request from {}:{}", client.Addr(), client.Port());
        WinspoolPrinterJobFactory jobFactory;
        RawPrintSession session(jobFactory, m_printerName);
        TcpRawPrintTransport transport(client);
        session.Run(transport);
        spdlog::info("Completed RAW print job from {}:{}", client.Addr(), client.Port());
    }
    catch (const std::exception& error) {
        spdlog::error("Error processing RAW print job from {}:{}: {}", client.Addr(), client.Port(), error.what());
    }
}
