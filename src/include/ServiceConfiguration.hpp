#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct RawServerConfiguration {
    std::string printerName;
    uint16_t port = 9100;
};

struct LpdQueueMapping {
    std::string queueName;
    std::string printerName;
};

enum class ServiceLogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

enum class ServiceConfigurationArgumentFormat {
    // Preserve the concise command line historically emitted by PrintHub.
    Compact,
    // Write every value-bearing configuration option, including defaults.
    Explicit,
};

// The complete set of service-hosting options.  Keeping this model independent
// from the console parser lets the MFC manager and PrintHub use one wire format.
struct ServiceConfiguration {
    static constexpr uint16_t kDefaultRawPort = 9100;
    static constexpr uint16_t kDefaultLpdPort = 515;
    static constexpr int kDefaultMinThreads = 1;

    std::vector<RawServerConfiguration> rawServers;
    std::vector<LpdQueueMapping> lpdQueues;
    int rawServerMinThreads = kDefaultMinThreads;
    uint16_t lpdPort = kDefaultLpdPort;
    std::string listenAddress = "0.0.0.0";
    bool snmpReject = false;
    int lpdMinThreads = kDefaultMinThreads;
    ServiceLogLevel logLevel = ServiceLogLevel::Info;

    // Parses only service configuration switches; process/service-management
    // switches are deliberately rejected so callers cannot lose them silently.
    static ServiceConfiguration Parse(const std::vector<std::string>& arguments, bool requireRunnableListener = true);
    std::vector<std::string> ToArguments() const;
    std::vector<std::string> ToArguments(ServiceConfigurationArgumentFormat format) const;
    void Validate(bool requireRunnableListener = true) const;

    static std::string LogLevelToString(ServiceLogLevel value);
    static ServiceLogLevel ParseLogLevel(const std::string& value);
    static bool IsIpv4Address(const std::string& value);
};

// Resolves an LPD queue using the same ASCII case-insensitive exact-match
// rule used by the service.  There is intentionally no Windows-printer-name
// fallback: only explicitly configured queues are accepted.
std::optional<std::string> ResolveLpdQueuePrinter(const std::vector<LpdQueueMapping>& mappings,
    std::string_view queueName);

// Windows-compatible argument quoting used for a CreateProcess command line.
std::wstring QuoteWindowsCommandLineArgument(const std::wstring& value);
std::wstring BuildWindowsCommandLine(const std::vector<std::wstring>& arguments);
