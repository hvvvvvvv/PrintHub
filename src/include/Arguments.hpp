#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <argparse.h>
#include <spdlog/spdlog.h>

class Arguments {
public:
    using RawServersConfigType = std::vector<std::tuple<std::string, uint16_t>>;

    static constexpr auto ARGS_RAW_SERVERS = "--raw-server";
    static constexpr auto ARGS_RAW_SERVER_MIN_THREADS = "--raw-server-min-threads";
    static constexpr auto ARGS_LPD_PORT = "--lpd-port";
    static constexpr auto ARGS_LPD_MIN_THREADS = "--lpd-min-threads";
    static constexpr auto ARGS_LPD_DISABLE = "--lpd-disable";
    static constexpr auto ARGS_LISTEN_ADDRESS = "--listen-address";
    static constexpr auto ARGS_SNMP_REJECT = "--snmp-reject";
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
    static constexpr uint16_t DEFAULT_RAW_PORT = 9100;
    static constexpr auto DEFAULT_LOG_LEVEL = spdlog::level::info;
    static constexpr auto DEFAULT_LPD_DISABLE = false;
    static constexpr auto DEFAULT_SNMP_REJECT = false;
    static constexpr auto DEFAULT_SILENT = false;
    static constexpr auto DEFAULT_LISTEN_ADDRESS = "0.0.0.0";
    static constexpr char RAW_SERVER_SEPARATOR = ':';
    static constexpr size_t RAW_SERVER_PORT_OFFSET = 1;
    static constexpr uint16_t MINIMUM_PORT = 1;

    Arguments();
    Arguments(const Arguments&) = delete;
    Arguments& operator=(const Arguments&) = delete;

    void Parse(int argc, char** argv);
    std::vector<std::string> GetRawArguments(std::optional<std::set<std::string>> ignoreItems) const;
    void PrintHelp() const;
    std::string Dump() const;

    std::optional<RawServersConfigType> RawServersConfig;
    int RawServerMinThreads;
    uint16_t LpdPort;
    bool LpdDisable;
    std::string ListenAddress;
    bool SnmpReject;
    int LpdMinThreads;
    spdlog::level::level_enum LogLevel;
    std::optional<std::string> ServiceOperation;
    bool Silent;

private:
    void parseRawServers(const std::vector<std::string>& rawServers);
    static spdlog::level::level_enum parseLogLevel(const std::string& value);
    static std::string logLevelToString(spdlog::level::level_enum level);

    argparse::ArgumentParser m_parser;
};
