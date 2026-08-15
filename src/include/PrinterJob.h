#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#include <fmt/format.h>
#include <windows.h>

#include <PrintJob.h>

class PrinterJob final : public IPrintJob {
    public:
        PrinterJob(const std::string& printerName, const std::string& docName);
        ~PrinterJob() override;
        PrinterJob(const PrinterJob&) = delete;
        PrinterJob& operator=(const PrinterJob&) = delete;
        void Write(const char* data, size_t size) override;
        void SetDocumentName(const std::string& docName) override;
        void Cancel() override;
        void Commit() override;
        size_t GetBytesWritten() const override { return m_bytesWritten; }
    private:
        std::string m_printerName;
        std::string m_dataType;
        std::string m_docName;
        HANDLE m_hPrinter = nullptr;
        DWORD m_jobId = 0;
        size_t m_bytesWritten = 0;

        void startDocPrinter();
};

class WinspoolPrinterJobFactory final : public IPrinterJobFactory {
public:
    std::unique_ptr<IPrintJob> Create(const std::string& printerName,
        const std::string& documentName) override;
};

class PrinterJobError : public std::runtime_error {
    public:
        PrinterJobError(const std::string &printerName, const std::string &message)
            : std::runtime_error(fmt::format("{} printer name {}", message, printerName)) {}
};
