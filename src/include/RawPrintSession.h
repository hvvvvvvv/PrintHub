#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include <PrintJob.h>

class IRawPrintTransport {
public:
    using BufferCallback = std::function<void(const char*, size_t)>;

    virtual ~IRawPrintTransport() = default;
    virtual void ReadToEnd(const BufferCallback& callback) = 0;
};

class RawPrintSession {
public:
    RawPrintSession(IPrinterJobFactory& jobFactory, std::string printerName);
    void Run(IRawPrintTransport& transport);

private:
    IPrinterJobFactory& m_jobFactory;
    std::string m_printerName;
};
