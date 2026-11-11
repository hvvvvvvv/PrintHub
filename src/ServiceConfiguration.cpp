#include <ServiceConfiguration.hpp>
#include <AsciiText.h>
#include <Ipv4.h>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace {

void validateCount(int value, const char* field) {
    if (value < 1 || value > UINT16_MAX) {
        throw std::invalid_argument(std::string(field) + ": expected integer 1..65535");
    }
}

void validateText(const std::string& value, const std::string& field) {
    if (value.empty()) throw std::invalid_argument(field + ": expected nonempty string");
    if (value.find('\0') != std::string::npos) {
        throw std::invalid_argument(field + ": embedded NUL is not allowed");
    }
}

struct Listener {
    uint32_t address;
    uint16_t port;
    std::string field;
    std::string endpoint;
};

void addListener(std::vector<Listener>& listeners, uint32_t address, uint16_t port,
    const std::string& field, const std::string& listenAddress) {
    const auto endpoint = listenAddress + ":" + std::to_string(port);
    for (const auto& other : listeners) {
        if (port == other.port && (address == other.address || address == 0 || other.address == 0)) {
            throw std::invalid_argument(field + ": listener " + endpoint + " conflicts with " +
                other.field + " at " + other.endpoint);
        }
    }
    listeners.push_back({address, port, field, endpoint});
}

} // namespace

void ServiceConfiguration::Validate() const {
    validateCount(rawServerMinThreads, "minThreads.raw");
    validateCount(lpdMinThreads, "minThreads.lpd");
    validateCount(maxPendingClients, "maxPendingClients");
    validateText(lpdServer.listenAddress, "lpdServer.listenAddress");
    const auto lpdAddress = ParseIpv4Address(lpdServer.listenAddress);
    if (!lpdAddress) {
        throw std::invalid_argument("lpdServer.listenAddress: expected IPv4 address");
    }
    if (lpdServer.port == 0) {
        throw std::invalid_argument("lpdServer.port: expected integer 1..65535");
    }
    if (!ppmDisabled && !printerMap.empty() && lpdServer.port != kDefaultLpdPort) {
        throw std::invalid_argument("lpdServer.port: PPM requires port 515; set ppmDisabled=true to use another LPD port");
    }

    std::set<std::string> queues;
    std::set<std::pair<uint32_t, uint16_t>> rawEndpoints;
    std::vector<Listener> listeners;
    if (!printerMap.empty()) {
        addListener(listeners, *lpdAddress, lpdServer.port, "lpdServer (LPD server)", lpdServer.listenAddress);
    }
    for (size_t index = 0; index < printerMap.size(); ++index) {
        const auto& mapping = printerMap[index];
        const auto field = "printerMap[" + std::to_string(index) + "]";
        validateText(mapping.name, field + ".name");
        validateText(mapping.lprQueue, field + ".lprQueue");
        if (mapping.lprQueue.find_first_of("\r\n") != std::string::npos) {
            throw std::invalid_argument(field + ".lprQueue: must not contain CR or LF");
        }
        if (!queues.insert(ascii::LowerCopy(mapping.lprQueue)).second) {
            throw std::invalid_argument(field + ".lprQueue: queue names must be unique (case-insensitive)");
        }
        if (!ppmDisabled && (mapping.lprQueue.size() > 32 ||
            !std::all_of(mapping.lprQueue.begin(), mapping.lprQueue.end(), [](unsigned char character) {
                return character >= 0x21 && character <= 0x7e && character != '=';
            }))) {
            throw std::invalid_argument(field + ".lprQueue: PPM requires 1..32 printable ASCII characters without whitespace or '='; set ppmDisabled=true to disable PPM");
        }
        if (!mapping.rawServer) continue;

        const auto& server = *mapping.rawServer;
        const auto rawField = field + ".rawServer";
        validateText(server.listenAddress, rawField + ".listenAddress");
        const auto address = ParseIpv4Address(server.listenAddress);
        if (!address) {
            throw std::invalid_argument(rawField + ".listenAddress: expected IPv4 address");
        }
        if (server.port == 0) {
            throw std::invalid_argument(rawField + ".port: expected integer 1..65535");
        }
        if (!rawEndpoints.insert({*address, server.port}).second) {
            throw std::invalid_argument(rawField + ": duplicate RAW endpoint " +
                server.listenAddress + ":" + std::to_string(server.port));
        }
        addListener(listeners, *address, server.port, rawField, server.listenAddress);
    }
    // Preserve listener conflict diagnostics before applying PPM-only restrictions.
    if (!ppmDisabled && !printerMap.empty()) {
        for (const auto& listener : listeners) {
            if (listener.address != *lpdAddress) {
                throw std::invalid_argument(listener.field +
                    ".listenAddress: PPM discovery requires all listeners to share the same listen address; set ppmDisabled=true to use different addresses");
            }
        }
    }
}

std::optional<std::string> ResolveLpdQueuePrinter(const std::vector<LpdQueueMapping>& mappings, std::string_view queueName) {
    for (const auto& mapping : mappings) {
        if (ascii::EqualsIgnoreCase(mapping.queueName, queueName)) return mapping.printerName;
    }
    return std::nullopt;
}

std::string ServiceConfiguration::LogLevelToString(ServiceLogLevel value) {
    switch (value) {
    case ServiceLogLevel::Trace: return "trace";
    case ServiceLogLevel::Debug: return "debug";
    case ServiceLogLevel::Info: return "info";
    case ServiceLogLevel::Warn: return "warn";
    case ServiceLogLevel::Error: return "error";
    case ServiceLogLevel::Critical: return "critical";
    case ServiceLogLevel::Off: return "off";
    }
    throw std::invalid_argument("Unknown log level");
}

ServiceLogLevel ServiceConfiguration::ParseLogLevel(const std::string& text) {
    const auto value = ascii::LowerCopy(text);
    if (value == "trace") return ServiceLogLevel::Trace;
    if (value == "debug") return ServiceLogLevel::Debug;
    if (value == "info") return ServiceLogLevel::Info;
    if (value == "warn") return ServiceLogLevel::Warn;
    if (value == "error") return ServiceLogLevel::Error;
    if (value == "critical") return ServiceLogLevel::Critical;
    if (value == "off") return ServiceLogLevel::Off;
    throw std::invalid_argument("Invalid log level: " + text);
}

bool ServiceConfiguration::IsIpv4Address(const std::string& value) {
    return ::IsIpv4Address(value);
}
