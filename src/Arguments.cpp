#include <Arguments.hpp>

#include <config.h>
#include <ServiceConfiguration.hpp>
#include <utility.h>

#include <cctype>
#include <fmt/format.h>
#include <iostream>
#include <map>
#include <stdexcept>

Arguments::Arguments() : m_parser(argparse::ArgumentParser(PROJECT_NAME, PROJECT_VERSION)) {
    m_parser.add_argument(ARGS_RAW_SERVERS).help("List of raw server printer name port maps").append();
    m_parser.add_argument(ARGS_LPD_QUEUE).help("LPD queue-to-printer map in queue=printer form").append();
    m_parser.add_argument(ARGS_RAW_SERVER_MIN_THREADS).help("Minimum threads for raw printing server")
        .default_value(DEFAULT_MIN_THREADS).scan<'d', uint16_t>();
    m_parser.add_argument(ARGS_MAX_PENDING_CLIENTS).help("Maximum pending TCP clients for each listener")
        .default_value(ServiceConfiguration::kDefaultMaxPendingClients).scan<'d', uint16_t>();
    m_parser.add_argument(ARGS_LPD_PORT).help("Port for LPD server")
        .default_value(DEFAULT_LPD_PORT).scan<'d', uint16_t>();
    m_parser.add_argument(ARGS_LPD_MAX_CONTROL_FILE_BYTES).help("Maximum bytes accepted for an LPD control file")
        .default_value(ServiceConfiguration::kDefaultLpdMaxControlFileBytes).scan<'d', uint64_t>();
    m_parser.add_argument(ARGS_LPD_MIN_THREADS).help("Minimum threads for LPD server")
        .default_value(DEFAULT_MIN_THREADS).scan<'d', uint16_t>();
    m_parser.add_argument(ARGS_LISTEN_ADDRESS).help("IPv4 address used by all RAW and LPD listeners")
        .default_value(std::string(DEFAULT_LISTEN_ADDRESS));
    m_parser.add_argument(ARGS_SNMP_REJECT).help("Reply immediately to SNMP v1/v2c discovery requests on UDP port 161")
        .default_value(DEFAULT_SNMP_REJECT).implicit_value(true);
    m_parser.add_argument(ARGS_SNMP_PPM).help("Publish printers, RAW/LPR ports, automatic device identity and queue status through SNMP on UDP 161 (exclusive with --snmp-reject)")
        .default_value(DEFAULT_SNMP_PPM).implicit_value(true);
    m_parser.add_argument(ARGS_LOG_LEVEL).help("Set log level, choices(trace | debug | info | warn | error | critical)")
        .default_value(DEFAULT_LOG_LEVEL).action(parseLogLevel);
    m_parser.add_argument(ARGS_SERVICE).help(fmt::format("Service management, choices({} | {} | {} | {} | {})",
        ARGS_SERVICE_INSTALL, ARGS_SERVICE_UNINSTALL, ARGS_SERVICE_START, ARGS_SERVICE_STOP, ARGS_SERVICE_RESTART))
        .choices(ARGS_SERVICE_INSTALL, ARGS_SERVICE_UNINSTALL, ARGS_SERVICE_START, ARGS_SERVICE_STOP, ARGS_SERVICE_RESTART);
    m_parser.add_argument(ARGS_SILENT).default_value(DEFAULT_SILENT).implicit_value(true).hidden();

    RawServersConfig = std::nullopt;
    LpdQueuesConfig = std::nullopt;
    RawServerMinThreads = DEFAULT_MIN_THREADS;
    MaxPendingClients = ServiceConfiguration::kDefaultMaxPendingClients;
    LpdPort = DEFAULT_LPD_PORT;
    LpdMaxControlFileBytes = ServiceConfiguration::kDefaultLpdMaxControlFileBytes;
    ListenAddress = DEFAULT_LISTEN_ADDRESS;
    SnmpReject = DEFAULT_SNMP_REJECT;
    SnmpPpm = DEFAULT_SNMP_PPM;
    LpdMinThreads = DEFAULT_MIN_THREADS;
    LogLevel = DEFAULT_LOG_LEVEL;
    ServiceOperation = std::nullopt;
    Silent = DEFAULT_SILENT;
}

void Arguments::Parse(int argc, char** argv) {
    m_parser.parse_args(argc, argv);

    // The console-only switches are parsed by argparse.  Everything persisted
    // in the service ImagePath is parsed by the shared configuration model so
    // the MFC manager and the server accept exactly the same syntax.
    std::vector<std::string> configurationArguments;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == ARGS_SERVICE) {
            ++index;
            continue;
        }
        if (argument == ARGS_SILENT) {
            continue;
        }
        configurationArguments.push_back(argument);
    }
    ServiceOperation = m_parser.is_used(ARGS_SERVICE)
        ? std::make_optional(m_parser.get<std::string>(ARGS_SERVICE)) : std::nullopt;
    const bool requireRunnableListener = !ServiceOperation.has_value() ||
        *ServiceOperation == ARGS_SERVICE_INSTALL;
    const auto configuration = ServiceConfiguration::Parse(configurationArguments, requireRunnableListener);
    RawServersConfig = RawServersConfigType{};
    for (const auto& server : configuration.rawServers) {
        RawServersConfig->emplace_back(server.printerName, server.port);
    }
    if (RawServersConfig->empty()) {
        RawServersConfig.reset();
    }
    LpdQueuesConfig = configuration.lpdQueues;
    if (LpdQueuesConfig->empty()) {
        LpdQueuesConfig.reset();
    }
    RawServerMinThreads = configuration.rawServerMinThreads;
    MaxPendingClients = configuration.maxPendingClients;
    LpdPort = configuration.lpdPort;
    LpdMaxControlFileBytes = configuration.lpdMaxControlFileBytes;
    ListenAddress = configuration.listenAddress;
    SnmpReject = configuration.snmpReject;
    SnmpPpm = configuration.snmpPpm;
    LpdMinThreads = configuration.lpdMinThreads;
    LogLevel = parseLogLevel(ServiceConfiguration::LogLevelToString(configuration.logLevel));
    Silent = m_parser.get<bool>(ARGS_SILENT);
}

std::vector<std::string> Arguments::GetRawArguments(std::optional<std::set<std::string>> ignoreItems) const {
    const auto isIgnored = [&ignoreItems](const std::string& item) {
        return ignoreItems.has_value() && ignoreItems->contains(item);
    };
    ServiceConfiguration configuration;
    if (RawServersConfig.has_value()) {
        for (const auto& server : *RawServersConfig) {
            configuration.rawServers.push_back({ std::get<0>(server), std::get<1>(server) });
        }
    }
    if (LpdQueuesConfig.has_value()) {
        configuration.lpdQueues = *LpdQueuesConfig;
    }
    configuration.rawServerMinThreads = RawServerMinThreads;
    configuration.maxPendingClients = MaxPendingClients;
    configuration.lpdPort = LpdPort;
    configuration.lpdMaxControlFileBytes = LpdMaxControlFileBytes;
    configuration.listenAddress = ListenAddress;
    configuration.snmpReject = SnmpReject;
    configuration.snmpPpm = SnmpPpm;
    configuration.lpdMinThreads = LpdMinThreads;
    configuration.logLevel = static_cast<ServiceLogLevel>(LogLevel);

    const auto allConfigurationArguments = configuration.ToArguments();
    std::vector<std::string> result;
    for (size_t index = 0; index < allConfigurationArguments.size(); ++index) {
        const auto& argument = allConfigurationArguments[index];
        if (isIgnored(argument)) {
            if (index + 1 < allConfigurationArguments.size() && allConfigurationArguments[index + 1].starts_with("--") == false) {
                ++index;
            }
            continue;
        }
        result.push_back(argument);
        if (index + 1 < allConfigurationArguments.size() && allConfigurationArguments[index + 1].starts_with("--") == false) {
            result.push_back(allConfigurationArguments[++index]);
        }
    }
    if (ServiceOperation.has_value() && !isIgnored(ARGS_SERVICE)) {
        result.push_back(ARGS_SERVICE);
        result.push_back(*ServiceOperation);
    }
    if (Silent != DEFAULT_SILENT && !isIgnored(ARGS_SILENT)) {
        result.push_back(ARGS_SILENT);
    }
    return result;
}

void Arguments::PrintHelp() const {
    std::cout << m_parser;
}

std::string Arguments::Dump() const {
    std::string result = fmt::format("RawServersConfig: {}\n", RawServersConfig.has_value() ? "set" : "not set");
    if (RawServersConfig.has_value()) {
        for (const auto& server : *RawServersConfig) {
            result.append(fmt::format("  - {}:{}\n", std::get<0>(server), std::get<1>(server)));
        }
    }
    result.append(fmt::format("LpdQueuesConfig: {}\n", LpdQueuesConfig.has_value() ? "set" : "not set"));
    if (LpdQueuesConfig.has_value()) {
        for (const auto& mapping : *LpdQueuesConfig) {
            result.append(fmt::format("  - {}={}\n", mapping.queueName, mapping.printerName));
        }
    }
    result.append(fmt::format("RawServerMinThreads: {}\nMaxPendingClients: {}\nLpdPort: {}\nLpdMaxControlFileBytes: {}\nListenAddress: {}\nSnmpReject: {}\nSnmpPpm: {}\nLpdMinThreads: {}\nLogLevel: {}\n",
        RawServerMinThreads, MaxPendingClients, LpdPort, LpdMaxControlFileBytes, ListenAddress, SnmpReject, SnmpPpm, LpdMinThreads, logLevelToString(LogLevel)));
    result.append(ServiceOperation.has_value() ? fmt::format("ServiceOperation: {}\n", *ServiceOperation) : "ServiceOperation: not set\n");
    result.append(fmt::format("Silent: {}", Silent));
    return result;
}

spdlog::level::level_enum Arguments::parseLogLevel(const std::string& value) {
    using spdlog::level::level_enum;
    static const std::map<std::string, level_enum> mapping{
        { "trace", level_enum::trace }, { "debug", level_enum::debug }, { "info", level_enum::info },
        { "warn", level_enum::warn }, { "warning", level_enum::warn }, { "error", level_enum::err },
        { "critical", level_enum::critical }, { "off", level_enum::off },
    };
    std::string normalized;
    normalized.reserve(value.size());
    for (const auto character : value) {
        normalized += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (const auto iterator = mapping.find(normalized); iterator != mapping.end()) {
        return iterator->second;
    }
    const auto numericLevel = utility::StringToNumber<int>(normalized);
    if (!numericLevel.has_value()) {
        throw std::invalid_argument("Invalid log level: " + value);
    }
    if (*numericLevel < static_cast<int>(level_enum::trace)) {
        return level_enum::trace;
    }
    if (*numericLevel > static_cast<int>(level_enum::off)) {
        return level_enum::off;
    }
    return static_cast<level_enum>(*numericLevel);
}

std::string Arguments::logLevelToString(spdlog::level::level_enum level) {
    switch (level) {
    case spdlog::level::trace: return "trace";
    case spdlog::level::debug: return "debug";
    case spdlog::level::info: return "info";
    case spdlog::level::warn: return "warn";
    case spdlog::level::err: return "error";
    case spdlog::level::critical: return "critical";
    case spdlog::level::off: return "off";
    default: return "unknown";
    }
}

void Arguments::parseRawServers(const std::vector<std::string>& rawServers) {
    RawServersConfig = RawServersConfigType{};
    for (const auto& rawServer : rawServers) {
        if (rawServer.empty()) {
            continue;
        }
        const auto separator = rawServer.find(RAW_SERVER_SEPARATOR);
        const auto printerName = rawServer.substr(0, separator);
        auto port = DEFAULT_RAW_PORT;
        if (separator != std::string::npos) {
            const auto parsedPort = utility::StringToNumber<uint16_t>(rawServer.substr(separator + RAW_SERVER_PORT_OFFSET));
            if (!parsedPort.has_value() || *parsedPort < MINIMUM_PORT) {
                throw std::invalid_argument(fmt::format("Invalid port number in raw server definition: {}", rawServer));
            }
            port = *parsedPort;
        }
        if (printerName.empty()) {
            throw std::invalid_argument(fmt::format("Printer name cannot be empty in raw server definition: {}", rawServer));
        }
        RawServersConfig->emplace_back(printerName, port);
    }
    if (RawServersConfig->empty()) {
        RawServersConfig.reset();
    }
}
