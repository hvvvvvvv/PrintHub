#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

#include <PrintJob.h>

struct LpdEndpoint {
    std::string address;
    uint16_t port;
};

class LpdTransportDisconnected : public std::runtime_error {
public:
    explicit LpdTransportDisconnected(const LpdEndpoint& endpoint)
        : std::runtime_error("LPD transport disconnected: " + endpoint.address + ":" + std::to_string(endpoint.port)) {}
};

class ILpdTransport {
public:
    using BufferCallback = std::function<void(const char*, size_t)>;

    virtual ~ILpdTransport() = default;
    virtual LpdEndpoint Endpoint() const = 0;
    virtual void WriteByte(char value) = 0;
    virtual void ReadLine(std::string& line) = 0;
    virtual void ReadExactly(size_t size, const BufferCallback& callback) = 0;
    virtual void ReadToEnd(const BufferCallback& callback) = 0;
};

class LpdSession {
public:
    using PrinterNameResolver = std::function<std::optional<std::string>(const std::string& queueName)>;

    LpdSession(IPrinterJobFactory& jobFactory, PrinterNameResolver printerNameResolver,
        size_t maxControlFileBytes = 4 * 1024 * 1024);
    void Run(ILpdTransport& transport);

private:
    void handleReceiveJob(const std::string& queueName, ILpdTransport& transport);

    IPrinterJobFactory& m_jobFactory;
    PrinterNameResolver m_printerNameResolver;
    size_t m_maxControlFileBytes;
};
