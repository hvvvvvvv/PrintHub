#include "PrinterJob.h"

#include <fmt/format.h>
#include <fmt/core.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <memory>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr DWORD kDriverInfoLevel = 2;
constexpr DWORD kJobInfoLevel = 1;
constexpr DWORD kV4PrinterDriverVersion = 4;
constexpr DWORD kNoJobId = 0;
constexpr DWORD kNoBytesWritten = 0;
constexpr char kRawDataType[] = "RAW";
constexpr char kXpsPassDataType[] = "XPS_PASS";

} // namespace

PrinterJob::PrinterJob(const std::string& printerName, const std::string& docName)
    : m_printerName(printerName), m_docName(docName) {
    PRINTER_DEFAULTSA defaults{};
    defaults.DesiredAccess = PRINTER_ACCESS_USE;
    if (!OpenPrinterA(const_cast<LPSTR>(m_printerName.c_str()), &m_hPrinter, &defaults)) {
        throw PrinterJobError(m_printerName,
            fmt::format("Failed to open printer. Error code: {}.", GetLastError()));
    }

    DWORD needed = kNoBytesWritten;
    GetPrinterDriverA(m_hPrinter, nullptr, kDriverInfoLevel, nullptr, kNoBytesWritten, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == kNoBytesWritten) {
        const auto error = GetLastError();
        ClosePrinter(m_hPrinter);
        m_hPrinter = nullptr;
        throw PrinterJobError(m_printerName,
            fmt::format("Failed to get printer driver info size. Error code: {}.", error));
    }

    std::vector<BYTE> buffer(needed);
    if (!GetPrinterDriverA(m_hPrinter, nullptr, kDriverInfoLevel, buffer.data(), needed, &needed)) {
        const auto error = GetLastError();
        ClosePrinter(m_hPrinter);
        m_hPrinter = nullptr;
        throw PrinterJobError(m_printerName,
            fmt::format("Failed to get printer driver info. Error code: {}.", error));
    }

    const auto* driverInfo = reinterpret_cast<const DRIVER_INFO_2A*>(buffer.data());
    m_dataType = driverInfo->cVersion == kV4PrinterDriverVersion ? kXpsPassDataType : kRawDataType;
    spdlog::debug("Printer {} driver version: {}, data type: {}", m_printerName, driverInfo->cVersion, m_dataType);

    try {
        startDocPrinter();
    }
    catch (...) {
        ClosePrinter(m_hPrinter);
        m_hPrinter = nullptr;
        throw;
    }
}

PrinterJob::~PrinterJob() {
    if (m_hPrinter && m_jobId) {
        try {
            Cancel();
        }
        catch (...) {
            // Destructors cannot report spooler cleanup failures.
        }
    }
    if (m_hPrinter) {
        ClosePrinter(m_hPrinter);
        m_hPrinter = nullptr;
    }
}

void PrinterJob::Write(const char* data, size_t size) {
    size_t totalWritten = kNoBytesWritten;
    while (totalWritten < size) {
        DWORD bytesWritten = kNoBytesWritten;
        const auto remaining = size - totalWritten;
        const auto chunkSize = static_cast<DWORD>((std::min)(remaining,
            static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
        if (!WritePrinter(m_hPrinter, const_cast<char*>(data + totalWritten), chunkSize, &bytesWritten)) {
            throw PrinterJobError(m_printerName,
                fmt::format("Failed to write data to printer. Error code: {}.", GetLastError()));
        }
        if (bytesWritten == kNoBytesWritten) {
            throw PrinterJobError(m_printerName, "Printer accepted a zero-length write");
        }
        totalWritten += bytesWritten;
        m_bytesWritten += bytesWritten;
    }
}

void PrinterJob::SetDocumentName(const std::string& docName) {
    if (docName.empty()) {
        return;
    }
    m_docName = docName;

    DWORD bytesNeeded = kNoBytesWritten;
    GetJobA(m_hPrinter, m_jobId, kJobInfoLevel, nullptr, kNoBytesWritten, &bytesNeeded);
    if (bytesNeeded == kNoBytesWritten || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        throw PrinterJobError(m_printerName,
            fmt::format("Failed to get job info size for job ID {}. Error code: {}.", m_jobId, GetLastError()));
    }
    std::vector<BYTE> jobInfoBuffer(bytesNeeded);
    if (!GetJobA(m_hPrinter, m_jobId, kJobInfoLevel, jobInfoBuffer.data(), bytesNeeded, &bytesNeeded)) {
        throw PrinterJobError(m_printerName,
            fmt::format("Failed to get job info for job ID {}. Error code: {}.", m_jobId, GetLastError()));
    }
    auto* jobInfo = reinterpret_cast<JOB_INFO_1A*>(jobInfoBuffer.data());
    jobInfo->pDocument = const_cast<LPSTR>(m_docName.c_str());
    jobInfo->Position = JOB_POSITION_UNSPECIFIED;
    if (!SetJobA(m_hPrinter, m_jobId, kJobInfoLevel, jobInfoBuffer.data(), kNoBytesWritten)) {
        throw PrinterJobError(m_printerName,
            fmt::format("Failed to set job name for job ID {}. Error code: {}.", m_jobId, GetLastError()));
    }
}

void PrinterJob::Commit() {
    if (!m_jobId) {
        return;
    }
    if (!EndDocPrinter(m_hPrinter)) {
        throw PrinterJobError(m_printerName,
            fmt::format("Failed to end document job. Error code: {}.", GetLastError()));
    }
    m_jobId = kNoJobId;
}

void PrinterJob::Cancel() {
    if (!m_jobId) {
        return;
    }
    if (!SetJobA(m_hPrinter, m_jobId, kNoBytesWritten, nullptr, JOB_CONTROL_DELETE)) {
        throw PrinterJobError(m_printerName,
            fmt::format("Failed to clear job. Error code: {}.", GetLastError()));
    }
    if (!EndDocPrinter(m_hPrinter)) {
        throw PrinterJobError(m_printerName,
            fmt::format("Failed to end cleared document job. Error code: {}.", GetLastError()));
    }
    m_jobId = kNoJobId;
    m_bytesWritten = kNoBytesWritten;
}

void PrinterJob::startDocPrinter() {
    DOC_INFO_1A docInfo{};
    docInfo.pDocName = const_cast<LPSTR>(m_docName.c_str());
    docInfo.pDatatype = const_cast<LPSTR>(m_dataType.c_str());
    m_jobId = StartDocPrinterA(m_hPrinter, kJobInfoLevel, reinterpret_cast<LPBYTE>(&docInfo));
    if (!m_jobId) {
        throw PrinterJobError(m_printerName,
            fmt::format("Failed to start document job. Error code: {}.", GetLastError()));
    }
}

std::unique_ptr<IPrintJob> WinspoolPrinterJobFactory::Create(const std::string& printerName,
    const std::string& documentName) {
    return std::make_unique<PrinterJob>(printerName, documentName);
}
