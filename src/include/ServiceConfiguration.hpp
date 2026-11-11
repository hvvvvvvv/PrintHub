#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct RawServerConfiguration {
    std::string listenAddress = "0.0.0.0";
    uint16_t port = 9100;
};

struct LpdServerConfiguration {
    std::string listenAddress = "0.0.0.0";
    uint16_t port = 515;
};

struct PrinterMapping {
    std::string name;
    std::string lprQueue;
    std::optional<RawServerConfiguration> rawServer;
};

struct LpdQueueMapping {
    std::string queueName;
    std::string printerName;
};

enum class ServiceLogLevel { Trace, Debug, Info, Warn, Error, Critical, Off };

struct ServiceConfiguration {
    static constexpr uint16_t kDefaultLpdPort = 515;
    static constexpr int kDefaultMinThreads = 1;
    static constexpr int kDefaultMaxPendingClients = 256;
    static constexpr size_t kDefaultLpdMaxControlFileBytes = 4 * 1024 * 1024;

    ServiceLogLevel logLevel = ServiceLogLevel::Info;
    int maxPendingClients = kDefaultMaxPendingClients;
    LpdServerConfiguration lpdServer;
    int rawServerMinThreads = kDefaultMinThreads;
    int lpdMinThreads = kDefaultMinThreads;
    std::vector<PrinterMapping> printerMap;
    bool ppmDisabled = false;

    void Validate() const;
    static std::string LogLevelToString(ServiceLogLevel value);
    static ServiceLogLevel ParseLogLevel(const std::string& value);
    static bool IsIpv4Address(const std::string& value);
};

std::optional<std::string> ResolveLpdQueuePrinter(const std::vector<LpdQueueMapping>& mappings,
    std::string_view queueName);
