#include <ServiceConfiguration.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <set>
#include <stdexcept>

namespace {

constexpr auto kRawServer = "--raw-server";
constexpr auto kLpdQueue = "--lpd-queue";
constexpr auto kRawServerMinThreads = "--raw-server-min-threads";
constexpr auto kMaxPendingClients = "--max-pending-clients";
constexpr auto kLpdPort = "--lpd-port";
constexpr auto kLpdMaxControlFileBytes = "--lpd-max-control-file-bytes";
constexpr auto kLpdMinThreads = "--lpd-min-threads";
constexpr auto kListenAddress = "--listen-address";
constexpr auto kSnmpReject = "--snmp-reject";
constexpr auto kLogLevel = "--log-level";

uint16_t parsePort(const std::string& text, const char* name) {
    unsigned int value = 0;
    const auto [position, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || position != text.data() + text.size() || value == 0 || value > UINT16_MAX) {
        throw std::invalid_argument(std::string("Invalid ") + name + ": " + text);
    }
    return static_cast<uint16_t>(value);
}

int parseThreadCount(const std::string& text, const char* name) {
    int value = 0;
    const auto [position, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || position != text.data() + text.size() || value < 1 || value > UINT16_MAX) {
        throw std::invalid_argument(std::string("Invalid ") + name + ": " + text);
    }
    return value;
}

size_t parseByteCount(const std::string& text, const char* name) {
    uint64_t value = 0;
    const auto [position, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || position != text.data() + text.size() || value == 0 ||
        value > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        throw std::invalid_argument(std::string("Invalid ") + name + ": " + text);
    }
    return static_cast<size_t>(value);
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool equalsAsciiCaseInsensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        const auto lower = [](unsigned char character) {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A'))
                                                        : static_cast<char>(character);
        };
        if (lower(static_cast<unsigned char>(left[index])) != lower(static_cast<unsigned char>(right[index]))) return false;
    }
    return true;
}

const std::string& requireValue(const std::vector<std::string>& arguments, size_t& position, const char* name) {
    if (++position >= arguments.size()) {
        throw std::invalid_argument(std::string("Missing value for ") + name);
    }
    return arguments[position];
}

} // namespace

ServiceConfiguration ServiceConfiguration::Parse(const std::vector<std::string>& arguments, bool requireRunnableListener) {
    ServiceConfiguration result;
    for (size_t position = 0; position < arguments.size(); ++position) {
        const auto& argument = arguments[position];
        if (argument == kRawServer) {
            const auto& value = requireValue(arguments, position, kRawServer);
            const auto separator = value.rfind(':');
            RawServerConfiguration server;
            if (separator == std::string::npos) {
                server.printerName = value;
            } else {
                server.printerName = value.substr(0, separator);
                server.port = parsePort(value.substr(separator + 1), "raw server port");
            }
            if (server.printerName.empty()) {
                throw std::invalid_argument("Printer name cannot be empty in raw server definition");
            }
            result.rawServers.push_back(std::move(server));
        } else if (argument == kLpdQueue) {
            const auto& value = requireValue(arguments, position, kLpdQueue);
            const auto separator = value.find('=');
            if (separator == std::string::npos || separator == 0 || separator + 1 == value.size()) {
                throw std::invalid_argument("LPD queue mapping must be queue=printer");
            }
            result.lpdQueues.push_back({ value.substr(0, separator), value.substr(separator + 1) });
        } else if (argument == kRawServerMinThreads) {
            result.rawServerMinThreads = parseThreadCount(requireValue(arguments, position, kRawServerMinThreads), "raw server minimum threads");
        } else if (argument == kMaxPendingClients) {
            result.maxPendingClients = parseThreadCount(requireValue(arguments, position, kMaxPendingClients), "maximum pending clients");
        } else if (argument == kLpdPort) {
            result.lpdPort = parsePort(requireValue(arguments, position, kLpdPort), "LPD port");
        } else if (argument == kLpdMaxControlFileBytes) {
            result.lpdMaxControlFileBytes = parseByteCount(requireValue(arguments, position, kLpdMaxControlFileBytes), "LPD maximum control file bytes");
        } else if (argument == kLpdMinThreads) {
            result.lpdMinThreads = parseThreadCount(requireValue(arguments, position, kLpdMinThreads), "LPD minimum threads");
        } else if (argument == kListenAddress) {
            result.listenAddress = requireValue(arguments, position, kListenAddress);
        } else if (argument == kSnmpReject) {
            result.snmpReject = true;
        } else if (argument == kLogLevel) {
            result.logLevel = ParseLogLevel(requireValue(arguments, position, kLogLevel));
        } else {
            throw std::invalid_argument("Unknown service configuration argument: " + argument);
        }
    }
    result.Validate(requireRunnableListener);
    return result;
}

std::vector<std::string> ServiceConfiguration::ToArguments() const {
    return ToArguments(ServiceConfigurationArgumentFormat::Compact);
}

std::vector<std::string> ServiceConfiguration::ToArguments(ServiceConfigurationArgumentFormat format) const {
    Validate();
    const bool includeDefaults = format == ServiceConfigurationArgumentFormat::Explicit;
    std::vector<std::string> result;
    for (const auto& server : rawServers) {
        result.push_back(kRawServer);
        result.push_back(server.printerName + ":" + std::to_string(server.port));
    }
    for (const auto& mapping : lpdQueues) {
        result.push_back(kLpdQueue);
        result.push_back(mapping.queueName + "=" + mapping.printerName);
    }
    if (includeDefaults || rawServerMinThreads != kDefaultMinThreads) {
        result.push_back(kRawServerMinThreads);
        result.push_back(std::to_string(rawServerMinThreads));
    }
    if (includeDefaults || maxPendingClients != kDefaultMaxPendingClients) {
        result.push_back(kMaxPendingClients);
        result.push_back(std::to_string(maxPendingClients));
    }
    if (includeDefaults || lpdPort != kDefaultLpdPort) {
        result.push_back(kLpdPort);
        result.push_back(std::to_string(lpdPort));
    }
    if (includeDefaults || lpdMaxControlFileBytes != kDefaultLpdMaxControlFileBytes) {
        result.push_back(kLpdMaxControlFileBytes);
        result.push_back(std::to_string(lpdMaxControlFileBytes));
    }
    if (includeDefaults || listenAddress != "0.0.0.0") {
        result.push_back(kListenAddress);
        result.push_back(listenAddress);
    }
    if (snmpReject) {
        result.push_back(kSnmpReject);
    }
    if (includeDefaults || lpdMinThreads != kDefaultMinThreads) {
        result.push_back(kLpdMinThreads);
        result.push_back(std::to_string(lpdMinThreads));
    }
    if (includeDefaults || logLevel != ServiceLogLevel::Info) {
        result.push_back(kLogLevel);
        result.push_back(LogLevelToString(logLevel));
    }
    return result;
}

void ServiceConfiguration::Validate(bool requireRunnableListener) const {
    if (rawServerMinThreads < 1 || lpdMinThreads < 1 || maxPendingClients < 1) {
        throw std::invalid_argument("Thread counts and maximum pending clients must be at least 1");
    }
    if (lpdMaxControlFileBytes == 0) {
        throw std::invalid_argument("LPD maximum control file bytes must be at least 1");
    }
    if (lpdPort == 0) {
        throw std::invalid_argument("LPD port cannot be zero");
    }
    if (!IsIpv4Address(listenAddress)) {
        throw std::invalid_argument("Listen address must be a valid IPv4 address");
    }
    if (requireRunnableListener && rawServers.empty() && lpdQueues.empty()) {
        throw std::invalid_argument("Configure at least one RAW printer or LPD queue");
    }
    std::set<uint16_t> ports;
    for (const auto& server : rawServers) {
        if (server.printerName.empty() || server.port == 0) {
            throw std::invalid_argument("RAW printer name and port are required");
        }
        if (!ports.insert(server.port).second) {
            throw std::invalid_argument("RAW listener ports must be unique");
        }
        if (!lpdQueues.empty() && server.port == lpdPort) {
            throw std::invalid_argument("RAW and LPD listeners cannot use the same port");
        }
    }
    for (size_t index = 0; index < lpdQueues.size(); ++index) {
        const auto& mapping = lpdQueues[index];
        if (mapping.queueName.empty() || mapping.printerName.empty() || mapping.queueName.find('=') != std::string::npos) {
            throw std::invalid_argument("LPD queue and printer name are required; queue name cannot contain '='");
        }
        for (size_t earlier = 0; earlier < index; ++earlier) {
            if (equalsAsciiCaseInsensitive(mapping.queueName, lpdQueues[earlier].queueName)) {
                throw std::invalid_argument("LPD queue names must be unique (case-insensitive)");
            }
        }
    }
}

std::optional<std::string> ResolveLpdQueuePrinter(const std::vector<LpdQueueMapping>& mappings, std::string_view queueName) {
    for (const auto& mapping : mappings) {
        if (equalsAsciiCaseInsensitive(mapping.queueName, queueName)) return mapping.printerName;
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
    const auto value = lowercase(text);
    if (value == "trace" || value == "0") return ServiceLogLevel::Trace;
    if (value == "debug" || value == "1") return ServiceLogLevel::Debug;
    if (value == "info" || value == "2") return ServiceLogLevel::Info;
    if (value == "warn" || value == "warning" || value == "3") return ServiceLogLevel::Warn;
    if (value == "error" || value == "4") return ServiceLogLevel::Error;
    if (value == "critical" || value == "5") return ServiceLogLevel::Critical;
    if (value == "off" || value == "6") return ServiceLogLevel::Off;
    throw std::invalid_argument("Invalid log level: " + text);
}

bool ServiceConfiguration::IsIpv4Address(const std::string& value) {
    size_t start = 0;
    for (int part = 0; part < 4; ++part) {
        const auto end = value.find('.', start);
        if ((part == 3 && end != std::string::npos) || (part != 3 && end == std::string::npos)) {
            return false;
        }
        const auto length = (end == std::string::npos ? value.size() : end) - start;
        if (length == 0 || length > 3) return false;
        unsigned int number = 0;
        const auto* first = value.data() + start;
        const auto* last = first + length;
        const auto [position, error] = std::from_chars(first, last, number);
        if (error != std::errc{} || position != last || number > 255) return false;
        start = end + 1;
    }
    return true;
}

std::wstring QuoteWindowsCommandLineArgument(const std::wstring& value) {
    if (value.empty()) return L"\"\"";
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    size_t slashCount = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashCount;
        } else if (character == L'\"') {
            result.append(slashCount * 2 + 1, L'\\');
            result.push_back(character);
            slashCount = 0;
        } else {
            result.append(slashCount, L'\\');
            result.push_back(character);
            slashCount = 0;
        }
    }
    result.append(slashCount * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring BuildWindowsCommandLine(const std::vector<std::wstring>& arguments) {
    std::wstring result;
    for (const auto& argument : arguments) {
        if (!result.empty()) result.push_back(L' ');
        result += QuoteWindowsCommandLineArgument(argument);
    }
    return result;
}
