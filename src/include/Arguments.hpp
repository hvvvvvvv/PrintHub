#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <argparse.h>
#include <ServiceConfiguration.hpp>

class Arguments {
public:
    using RawServersConfigType = std::vector<std::tuple<std::string, uint16_t>>;
    using LpdQueuesConfigType = std::vector<LpdQueueMapping>;

    static constexpr auto ARGS_RAW_SERVERS = "--raw-server";
    static constexpr auto ARGS_RAW_SERVER_MIN_THREADS = "--raw-server-min-threads";
    static constexpr auto ARGS_MAX_PENDING_CLIENTS = "--max-pending-clients";
    static constexpr auto ARGS_LPD_QUEUE = "--lpd-queue";
    static constexpr auto ARGS_LPD_PORT = "--lpd-port";
    static constexpr auto ARGS_LPD_MAX_CONTROL_FILE_BYTES = "--lpd-max-control-file-bytes";
    static constexpr auto ARGS_LPD_MIN_THREADS = "--lpd-min-threads";
    static constexpr auto ARGS_LISTEN_ADDRESS = "--listen-address";
    static constexpr auto ARGS_SNMP_PPM = "--snmp-ppm";
    static constexpr auto ARGS_LOG_LEVEL = "--log-level";
    static constexpr auto ARGS_SERVICE = "--service";
    static constexpr auto ARGS_SILENT = "--silent";
    static constexpr auto ARGS_SERVICE_INSTALL = "install";
    static constexpr auto ARGS_SERVICE_UNINSTALL = "uninstall";
    static constexpr auto ARGS_SERVICE_START = "start";
    static constexpr auto ARGS_SERVICE_STOP = "stop";
    static constexpr auto ARGS_SERVICE_RESTART = "restart";

    static constexpr uint16_t DEFAULT_MIN_THREADS = 1;
    static constexpr uint16_t DEFAULT_LPD_PORT = 515;
    static constexpr auto DEFAULT_SNMP_PPM = false;
    static constexpr auto DEFAULT_LISTEN_ADDRESS = "0.0.0.0";

    Arguments();
    Arguments(const Arguments&) = delete;
    Arguments& operator=(const Arguments&) = delete;

    void Parse(int argc, char** argv);
    std::vector<std::string> GetRawArguments() const;
    void PrintHelp() const;
    std::string Dump() const;

    std::optional<RawServersConfigType> RawServersConfig;
    std::optional<LpdQueuesConfigType> LpdQueuesConfig;
    int RawServerMinThreads;
    int MaxPendingClients;
    uint16_t LpdPort;
    size_t LpdMaxControlFileBytes;
    std::string ListenAddress;
    bool SnmpPpm;
    int LpdMinThreads;
    ServiceLogLevel LogLevel;
    std::optional<std::string> ServiceOperation;

private:
    argparse::ArgumentParser m_parser;
};
