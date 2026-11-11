#include <Arguments.hpp>

#include "TestSupport.h"

#include <tuple>
#include <vector>

namespace {

void parse(Arguments& target, std::vector<std::string> options) {
    options.insert(options.begin(), "PrintHub-test");
    std::vector<char*> argv;
    for (auto& option : options) argv.push_back(option.data());
    const auto argc = static_cast<int>(argv.size());
    argv.push_back(nullptr);
    target.Parse(argc, argv.data());
}

void requireDefaultScalars(const Arguments& arguments) {
    require(arguments.RawServerMinThreads == 1 && arguments.LpdMinThreads == 1, "Default thread counts");
    require(arguments.MaxPendingClients == 256, "Default pending client count");
    require(arguments.LpdPort == 515 && arguments.LpdMaxControlFileBytes == 4194304, "Default LPD limits");
    require(arguments.ListenAddress == "0.0.0.0", "Default listen address");
    require(!arguments.SnmpReject && !arguments.SnmpPpm, "Default SNMP modes");
    require(arguments.LogLevel == ServiceLogLevel::Info, "Default log level");
}

void testDefaultsAndMinimalParse() {
    Arguments initial;
    requireDefaultScalars(initial);
    require(!initial.RawServersConfig && !initial.LpdQueuesConfig && !initial.ServiceOperation, "Initial optionals");
    requireThrows([&] { initial.GetRawArguments(); }, "Unconfigured GetRawArguments must validate");

    Arguments raw;
    parse(raw, {"--raw-server", "Office Printer"});
    requireDefaultScalars(raw);
    require(raw.RawServersConfig && *raw.RawServersConfig ==
        Arguments::RawServersConfigType{{"Office Printer", 9100}}, "Default RAW port");
    require(!raw.LpdQueuesConfig && !raw.ServiceOperation, "Unused optionals stay empty");
    require(raw.GetRawArguments() == std::vector<std::string>{"--raw-server", "Office Printer:9100"},
        "GetRawArguments is canonical service configuration");

    Arguments lpd;
    parse(lpd, {"--lpd-queue", "Queue=Printer"});
    requireDefaultScalars(lpd);
    require(!lpd.RawServersConfig && lpd.LpdQueuesConfig && lpd.LpdQueuesConfig->size() == 1, "LPD-only optionals");
    require(lpd.LpdQueuesConfig->front().queueName == "Queue" &&
        lpd.LpdQueuesConfig->front().printerName == "Printer", "LPD-only mapping");
}

void testFullParseAndRawArguments() {
    const std::string printer = "Office \u6253\u5370\u673A";
    Arguments arguments;
    parse(arguments, {
        "--service", "install", "--silent", "--log-level", "WaRnInG",
        "--raw-server", printer + ":9101", "--raw-server", "Second:9200",
        "--lpd-queue", "Queue=" + printer, "--lpd-queue", "Reports=Second",
        "--raw-server-min-threads", "2", "--max-pending-clients", "512",
        "--lpd-port", "1515", "--lpd-max-control-file-bytes", "123456",
        "--listen-address", "127.0.0.1", "--snmp-reject", "--lpd-min-threads", "4"
    });
    require(arguments.RawServersConfig && *arguments.RawServersConfig ==
        Arguments::RawServersConfigType{{printer, 9101}, {"Second", 9200}}, "All RAW mappings");
    require(arguments.LpdQueuesConfig && arguments.LpdQueuesConfig->size() == 2, "All LPD mappings");
    require((*arguments.LpdQueuesConfig)[0].queueName == "Queue" &&
        (*arguments.LpdQueuesConfig)[0].printerName == printer &&
        (*arguments.LpdQueuesConfig)[1].queueName == "Reports" &&
        (*arguments.LpdQueuesConfig)[1].printerName == "Second", "LPD mapping order and Unicode");
    require(arguments.RawServerMinThreads == 2 && arguments.MaxPendingClients == 512 &&
        arguments.LpdMinThreads == 4, "Explicit thread/client values");
    require(arguments.LpdPort == 1515 && arguments.LpdMaxControlFileBytes == 123456, "Explicit LPD values");
    require(arguments.ListenAddress == "127.0.0.1" && arguments.SnmpReject && !arguments.SnmpPpm &&
        arguments.LogLevel == ServiceLogLevel::Warn, "Explicit address, flags and log alias");
    require(arguments.ServiceOperation == std::optional<std::string>{"install"}, "Service action");
    const std::vector<std::string> expected{
        "--raw-server", printer + ":9101", "--raw-server", "Second:9200",
        "--lpd-queue", "Queue=" + printer, "--lpd-queue", "Reports=Second",
        "--raw-server-min-threads", "2", "--max-pending-clients", "512", "--lpd-port", "1515",
        "--lpd-max-control-file-bytes", "123456", "--listen-address", "127.0.0.1",
        "--snmp-reject", "--lpd-min-threads", "4", "--log-level", "warn"
    };
    require(arguments.GetRawArguments() == expected, "Process switches must not be persisted");
    Arguments reparsed;
    parse(reparsed, expected);
    require(reparsed.GetRawArguments() == expected && !reparsed.ServiceOperation, "Canonical round trip");
}

void testProcessSwitchesAndSafeFailures() {
    for (const auto* action : {"uninstall", "start", "stop", "restart"}) {
        Arguments arguments;
        parse(arguments, {"--service", action, "--silent"});
        requireDefaultScalars(arguments);
        require(arguments.ServiceOperation == std::optional<std::string>{action}, "Listenerless service action");
        require(!arguments.RawServersConfig && !arguments.LpdQueuesConfig, "Service command must not invent listeners");
        requireThrows([&] { arguments.GetRawArguments(); }, "GetRawArguments still requires a listener");
    }
    Arguments ppm;
    parse(ppm, {"--lpd-queue", "Queue=Printer", "--snmp-ppm", "--log-level", "6", "--silent"});
    require(ppm.SnmpPpm && !ppm.SnmpReject && ppm.LogLevel == ServiceLogLevel::Off, "PPM and numeric log alias");
    require(ppm.GetRawArguments() ==
        std::vector<std::string>{"--lpd-queue", "Queue=Printer", "--snmp-ppm", "--log-level", "off"},
        "PPM canonical arguments");

    // argparse::parse_args throws for these errors. Its built-in --help/-h and
    // --version/-v actions call std::exit(0), so those actions are never invoked.
    const std::vector<std::vector<std::string>> invalid{
        {}, {"--silent"}, {"--service", "install"}, {"--service", "invalid"},
        {"--unknown"}, {"--raw-server"}, {"--raw-server", "P:0"},
        {"--service", "stop", "--listen-address", "invalid"},
        {"--raw-server", "P", "--snmp-ppm", "--snmp-reject"}
    };
    for (size_t index = 0; index < invalid.size(); ++index) {
        Arguments arguments;
        requireThrows<std::exception>([&] { parse(arguments, invalid[index]); },
            "Safe invalid command case " + std::to_string(index) + " accepted");
    }
}

} // namespace

int main() {
    try {
        runTest("defaults and minimal parse", testDefaultsAndMinimalParse);
        runTest("full parse and raw arguments", testFullParseAndRawArguments);
        runTest("process switches and non-exiting failures", testProcessSwitchesAndSafeFailures);
    } catch (const std::exception& error) {
        std::cerr << "Arguments tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
