#include <Windows.h>
#include "PrinterJob.h"
#include <cstddef>
#include <fmt/format.h>
#include <string>
#include <winspool.h>
#include <fmt/core.h>


PrinterJob::PrinterJob(const std::string& printerName, const std::string& docName) {
	m_printerName = printerName;
	m_docName = docName;
    PRINTER_DEFAULTS pd = { NULL, NULL, PRINTER_ACCESS_ADMINISTER };
    if (!OpenPrinter(const_cast<LPSTR>(m_printerName.c_str()), &m_hPrinter, &pd)) {
        throw PrinterJobError(
            m_printerName,
            fmt::format("Failed to open printer {}. Error code: {}.",
                        m_printerName, GetLastError()));
    }
    DWORD needed;
    GetPrinterDriver(m_hPrinter, NULL, 2, NULL, 0, &needed);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        ClosePrinter(m_hPrinter);
        m_hPrinter = NULL;
        throw PrinterJobError(
            m_printerName,
            fmt::format("Failed to get printer driver info size for printer {}. Error code: {}.",
				m_printerName, GetLastError()));
    }
    std::vector<char> buffer(needed);
    if (::GetPrinterDriver(m_hPrinter, NULL, 2, (LPBYTE)&buffer[0], needed, &needed) == 0) {
        ClosePrinter(m_hPrinter);
        m_hPrinter = NULL;
        throw PrinterJobError(
            m_printerName,
            fmt::format("Failed to get printer driver info for printer {}. Error code: {}.",
				m_printerName, GetLastError()));
    }
	auto driverInfo = reinterpret_cast<DRIVER_INFO_2*>(&buffer[0]);
	m_dataType = driverInfo->cVersion == 4 ? "XPS_PASS" : "RAW";

    spdlog::debug("Printer {} driver version: {}, data type: {}", 
		m_printerName, driverInfo->cVersion, m_dataType);

    try {
        m_startDocPrinter();
    } catch (const std::exception &e) {
        ClosePrinter(m_hPrinter);
        m_hPrinter = NULL;
        throw e;
    }
}

PrinterJob::~PrinterJob() {
    if(m_jobId) {
        SetJob(m_hPrinter, m_jobId, 0, NULL, JOB_CONTROL_DELETE);
        EndDocPrinter(m_hPrinter);
    }
    if (m_hPrinter) {
        ClosePrinter(m_hPrinter);
        m_hPrinter = NULL;
    }
}

void PrinterJob::Write(char *data, DWORD size) {
    DWORD bytesWritten;
    if (!WritePrinter(m_hPrinter, data, size, &bytesWritten)) {
        throw PrinterJobError(
            m_printerName,
            fmt::format("Failed to write data to printer {}. Error code: {}.",
                        m_printerName, GetLastError()));
    }
    if (bytesWritten != size) {
        throw PrinterJobError(
            m_printerName,
            fmt::format(
                "Failed to write all data to printer {}. Expected: {}, Actual: {}.",
                m_printerName, size, bytesWritten));
    }
    m_bytesWritten += bytesWritten;
}

void PrinterJob::SetDocName(const std::string &docName) {
    DWORD bytesNeeded = 0;
    GetJob(m_hPrinter, m_jobId, 1, NULL, 0, &bytesNeeded);
    if (bytesNeeded <= 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        throw PrinterJobError(m_printerName, fmt::format("Failed to get job info size for job ID {}, error code {}.", m_jobId, GetLastError()));
    }
    auto jobInfoBuffer = std::make_unique<BYTE[]>(bytesNeeded);
    if (!GetJob(m_hPrinter, m_jobId, 1, jobInfoBuffer.get(), bytesNeeded, &bytesNeeded)) {
        throw PrinterJobError(m_printerName, fmt::format("Failed to get job info for job ID {}, error code {}.", m_jobId, GetLastError()));
    }
    auto jobInfo = reinterpret_cast<JOB_INFO_1*>(jobInfoBuffer.get());
    jobInfo->pDocument = const_cast<LPSTR>(m_docName.c_str());
    jobInfo->Position = JOB_POSITION_UNSPECIFIED;
    if (!SetJob(m_hPrinter, m_jobId, 1, jobInfoBuffer.get(), 0)) {
        throw PrinterJobError(m_printerName, fmt::format("Failed to set job name for job ID {}, error code {}.", m_jobId, GetLastError()));
    }
}

void PrinterJob::Print() {
    if (!EndDocPrinter(m_hPrinter)) {
        throw PrinterJobError(m_printerName, fmt::format("Failed to end document job for printer {}, error code {}.", m_printerName, GetLastError()));
    }

    m_jobId = 0;
}

void PrinterJob::Clear() {
    if(!m_bytesWritten) {
        return;
    }
    if(!SetJob(m_hPrinter, m_jobId, 0, NULL, JOB_CONTROL_DELETE)) {
        throw PrinterJobError(m_printerName, fmt::format("Failed to clear job for printer {}, error code {}.", m_printerName, GetLastError()));
    }     
    if(!EndDocPrinter(m_hPrinter)) {
        throw PrinterJobError(m_printerName, fmt::format("Failed to end document job for printer {}, error code {}.", m_printerName, GetLastError()));
    }
    m_startDocPrinter();
    m_jobId = 0;
}

void PrinterJob::m_startDocPrinter() {
    DOC_INFO_1 docInfo;
    docInfo.pDocName = const_cast<LPSTR>(m_docName.c_str());
    docInfo.pOutputFile = NULL;
    docInfo.pDatatype = const_cast<LPSTR>(m_dataType.c_str());
    
    m_jobId = StartDocPrinter(m_hPrinter, 1, reinterpret_cast<LPBYTE>(&docInfo));
    if (!m_jobId) {
        throw PrinterJobError(m_printerName, "Failed to start document job");
    }
}
