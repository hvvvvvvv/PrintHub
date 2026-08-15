#include <Server.h>
#include <spdlog/spdlog.h>
#include <PrinterJob.h>

RawPrinting::RawPrinting(std::string printerName, uint16_t port, int minThreads)
	: PrintServer(port, minThreads), m_printerName(std::move(printerName)) {
	if (m_printerName.empty()) {
		throw std::invalid_argument("Printer name cannot be empty");
	}
}

void RawPrinting::handleClient(TcpClient& client) {
	std::unique_ptr<PrinterJob> job = nullptr;
	try {
		job = std::make_unique<PrinterJob>(m_printerName, "raw_job");
	} catch (const PrinterJobError& e) {
		spdlog::error("Failed to create printer job for {}:{}. Error: {}", client.Addr(), client.Port(), e.what());
		return;
	}

	try {
		size_t totalBytes = 0;
		spdlog::info("Received raw print job request from {}:{}", client.Addr(), client.Port());
		auto cb = [&job, &totalBytes](const char* data, size_t size) {
			job->Write(const_cast<char*>(data), static_cast<DWORD>(size));
			totalBytes += size;
		};
		client.ReadAll(cb);
		spdlog::info("Received {} bytes of raw data from {}:{}", totalBytes, client.Addr(), client.Port());
		job->Print();
		spdlog::info("Successfully printed {} bytes to printer '{}'", job->GetBytesWritten(), m_printerName);
		
	} catch (const std::exception& e) {
		spdlog::error("Error processing print job from {}:{}. Error: {}", client.Addr(), client.Port(), e.what());
	}

}