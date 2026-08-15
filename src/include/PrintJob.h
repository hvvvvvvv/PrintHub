#pragma once

#include <cstddef>
#include <memory>
#include <string>

class IPrintJob {
public:
    virtual ~IPrintJob() = default;
    virtual void Write(const char* data, size_t size) = 0;
    virtual void SetDocumentName(const std::string& documentName) = 0;
    virtual void Cancel() = 0;
    virtual void Commit() = 0;
    virtual size_t GetBytesWritten() const = 0;
};

class IPrinterJobFactory {
public:
    virtual ~IPrinterJobFactory() = default;
    virtual std::unique_ptr<IPrintJob> Create(const std::string& printerName,
        const std::string& documentName) = 0;
};
