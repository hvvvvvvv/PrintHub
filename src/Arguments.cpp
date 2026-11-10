#include <Arguments.hpp>

#include <config.h>
#include <ServiceConfiguration.hpp>

#include <fmt/format.h>
#include <iostream>

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
        .default_value("info");
    m_parser.add_argument(ARGS_SERVICE).help(fmt::format("Service management, choices({} | {} | {} | {} | {})",
        ARGS_SERVICE_INSTALL, ARGS_SERVICE_UNINSTALL, ARGS_SERVICE_START, ARGS_SERVICE_STOP, ARGS_SERVICE_RESTART))
        .choices(ARGS_SERVICE_INSTALL, ARGS_SERVICE_UNINSTALL, ARGS_SERVICE_START, ARGS_SERVICE_STOP, ARGS_SERVICE_RESTART);
    m_parser.add_argument(ARGS_SILENT).default_value(false).implicit_value(true).hidden();

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
    LogLevel = ServiceLogLevel::Info;
    ServiceOperation = std::nullopt;
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
    LogLevel = configuration.logLevel;
}

std::vector<std::string> Arguments::GetRawArguments() const {
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
    configuration.logLevel = LogLevel;
    return configuration.ToArguments();
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
        RawServerMinThreads, MaxPendingClients, LpdPort, LpdMaxControlFileBytes, ListenAddress, SnmpReject, SnmpPpm, LpdMinThreads, ServiceConfiguration::LogLevelToString(LogLevel)));
    result.append(ServiceOperation.has_value() ? fmt::format("ServiceOperation: {}\n", *ServiceOperation) : "ServiceOperation: not set\n");
    return result;
}
