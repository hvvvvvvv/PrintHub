#include <RawPrintSession.h>

#include <stdexcept>
#include <utility>

RawPrintSession::RawPrintSession(IPrinterJobFactory& jobFactory, std::string printerName)
    : m_jobFactory(jobFactory), m_printerName(std::move(printerName)) {
    if (m_printerName.empty()) {
        throw std::invalid_argument("Printer name cannot be empty");
    }
}

void RawPrintSession::Run(IRawPrintTransport& transport) {
    auto job = m_jobFactory.Create(m_printerName, "raw_job");
    size_t bytesReceived = 0;
    transport.ReadToEnd([&job, &bytesReceived](const char* data, size_t size) {
        job->Write(data, size);
        bytesReceived += size;
    });
    if (bytesReceived != 0) {
        job->Commit();
    }
    else {
        job->Cancel();
    }
}
