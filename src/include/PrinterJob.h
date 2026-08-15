#include <string>
#include <windows.h>
#include <fmt/format.h>
#include <minwindef.h>
#include <winspool.h>
#include <spdlog/spdlog.h>

class PrinterJob {
    public:
        PrinterJob(const std::string& printerName, const std::string& docName);
        ~PrinterJob();
        void Write(char *data, DWORD size);
        void SetDocName(const std::string &docName);
        void Clear();
        void Print();
        DWORD GetBytesWritten() const { return m_bytesWritten; }
    private:
        std::string m_printerName;
        std::string m_dataType;
        std::string m_docName;
        HANDLE m_hPrinter;
        DWORD m_jobId;
        DWORD m_bytesWritten = 0;

        void m_startDocPrinter();
};

class PrinterJobError : public std::runtime_error {
    public:
        PrinterJobError(const std::string &printerName, const std::string &message)
            : std::runtime_error(fmt::format("{} printer name {}", message, printerName)) {}
};
