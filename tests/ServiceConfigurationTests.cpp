#include <ServiceConfiguration.hpp>

#include "TestSupport.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

namespace {

using Format = ServiceConfigurationArgumentFormat;
using Arguments = std::vector<std::string>;

void requireSame(const ServiceConfiguration& actual, const ServiceConfiguration& expected) {
    require(actual.rawServers.size() == expected.rawServers.size(), "RAW mapping count");
    for (size_t index = 0; index < actual.rawServers.size(); ++index) {
        require(actual.rawServers[index].printerName == expected.rawServers[index].printerName &&
            actual.rawServers[index].port == expected.rawServers[index].port, "RAW mapping/order");
    }
    require(actual.lpdQueues.size() == expected.lpdQueues.size(), "LPD mapping count");
    for (size_t index = 0; index < actual.lpdQueues.size(); ++index) {
        require(actual.lpdQueues[index].queueName == expected.lpdQueues[index].queueName &&
            actual.lpdQueues[index].printerName == expected.lpdQueues[index].printerName, "LPD mapping/order");
    }
    require(actual.rawServerMinThreads == expected.rawServerMinThreads, "RAW threads");
    require(actual.maxPendingClients == expected.maxPendingClients, "Pending client limit");
    require(actual.lpdPort == expected.lpdPort, "LPD port");
    require(actual.lpdMaxControlFileBytes == expected.lpdMaxControlFileBytes, "Control file limit");
    require(actual.listenAddress == expected.listenAddress, "Listen address");
    require(actual.snmpPpm == expected.snmpPpm, "SNMP flag");
    require(actual.lpdMinThreads == expected.lpdMinThreads, "LPD threads");
    require(actual.logLevel == expected.logLevel, "Log level");
}

void testDefaults() {
    const ServiceConfiguration value;
    require(ServiceConfiguration::kDefaultLpdPort == 515 && value.lpdPort == 515, "Default LPD port");
    require(ServiceConfiguration::kDefaultMinThreads == 1 && value.rawServerMinThreads == 1 &&
        value.lpdMinThreads == 1, "Default threads");
    require(ServiceConfiguration::kDefaultMaxPendingClients == 256 && value.maxPendingClients == 256,
        "Default pending clients");
    require(ServiceConfiguration::kDefaultLpdMaxControlFileBytes == 4194304 &&
        value.lpdMaxControlFileBytes == 4194304, "Default control limit");
    require(value.rawServers.empty() && value.lpdQueues.empty(), "Default mappings");
    require(value.listenAddress == "0.0.0.0" && !value.snmpPpm &&
        value.logLevel == ServiceLogLevel::Info, "Default address, SNMP and logging");
    requireSame(ServiceConfiguration::Parse({}, false), value);
    value.Validate(false);
    requireThrows([&] { value.Validate(); }, "No listener must fail Validate");
    requireThrows([] { ServiceConfiguration::Parse({}); }, "No listener must fail Parse");
    requireThrows([&] { value.ToArguments(); }, "Serialization requires a runnable configuration");
}

void testFullParseAndRoundTrips() {
    const std::string unicode = "Office \u6253\u5370\u673A";
    const Arguments input{
        "--log-level", "DeBuG", "--lpd-min-threads", "3",
        "--listen-address", "192.168.1.20", "--lpd-max-control-file-bytes", "1048576",
        "--lpd-port", "1515", "--max-pending-clients", "512", "--raw-server-min-threads", "2",
        "--lpd-queue", "Office=" + unicode, "--lpd-queue", "Reports=Printer=Two",
        "--raw-server", unicode, "--raw-server", "Server:Printer:9200"
    };
    ServiceConfiguration expected;
    expected.rawServers = {{unicode, 9100}, {"Server:Printer", 9200}};
    expected.lpdQueues = {{"Office", unicode}, {"Reports", "Printer=Two"}};
    expected.rawServerMinThreads = 2;
    expected.maxPendingClients = 512;
    expected.lpdPort = 1515;
    expected.lpdMaxControlFileBytes = 1048576;
    expected.listenAddress = "192.168.1.20";
    expected.lpdMinThreads = 3;
    expected.logLevel = ServiceLogLevel::Debug;
    const auto parsed = ServiceConfiguration::Parse(input);
    requireSame(parsed, expected);
    const Arguments canonical{
        "--raw-server", unicode + ":9100", "--raw-server", "Server:Printer:9200",
        "--lpd-queue", "Office=" + unicode, "--lpd-queue", "Reports=Printer=Two",
        "--raw-server-min-threads", "2", "--max-pending-clients", "512", "--lpd-port", "1515",
        "--lpd-max-control-file-bytes", "1048576", "--listen-address", "192.168.1.20",
        "--lpd-min-threads", "3", "--log-level", "debug"
    };
    for (const auto format : {Format::Compact, Format::Explicit}) {
        require(parsed.ToArguments(format) == canonical, "Full serialization order/values");
        requireSame(ServiceConfiguration::Parse(parsed.ToArguments(format)), expected);
    }
    require(parsed.ToArguments() == canonical, "No-argument overload must use Compact");
    const auto repeated = ServiceConfiguration::Parse({"--raw-server", "P", "--log-level", "warn",
        "--log-level", "OFF", "--max-pending-clients", "3", "--max-pending-clients", "4"});
    require(repeated.logLevel == ServiceLogLevel::Off && repeated.maxPendingClients == 4,
        "Repeated scalar options use the last value");
}

void testSerializationDefaultsAndFlags() {
    ServiceConfiguration value;
    value.rawServers = {{"Printer", 9100}};
    value.lpdQueues = {{"Queue", "Printer"}};
    const Arguments compact{"--raw-server", "Printer:9100", "--lpd-queue", "Queue=Printer"};
    const Arguments explicitDefaults{
        "--raw-server", "Printer:9100", "--lpd-queue", "Queue=Printer",
        "--raw-server-min-threads", "1", "--max-pending-clients", "256", "--lpd-port", "515",
        "--lpd-max-control-file-bytes", "4194304", "--listen-address", "0.0.0.0",
        "--lpd-min-threads", "1", "--log-level", "info"
    };
    require(value.ToArguments() == compact, "Compact must omit defaults and false flags");
    require(value.ToArguments(Format::Explicit) == explicitDefaults, "Explicit must emit every scalar default");
    for (const auto format : {Format::Compact, Format::Explicit}) {
        requireSame(ServiceConfiguration::Parse(value.ToArguments(format)), value);
    }
    value.snmpPpm = true;
    value.maxPendingClients = 17;
    value.logLevel = ServiceLogLevel::Off;
    const Arguments mixedCompact{
        "--raw-server", "Printer:9100", "--lpd-queue", "Queue=Printer",
        "--max-pending-clients", "17", "--snmp-ppm", "--log-level", "off"
    };
    const Arguments mixedExplicit{
        "--raw-server", "Printer:9100", "--lpd-queue", "Queue=Printer",
        "--raw-server-min-threads", "1", "--max-pending-clients", "17", "--lpd-port", "515",
        "--lpd-max-control-file-bytes", "4194304", "--listen-address", "0.0.0.0",
        "--snmp-ppm", "--lpd-min-threads", "1", "--log-level", "off"
    };
    require(value.ToArguments() == mixedCompact, "Mixed Compact serialization");
    require(value.ToArguments(Format::Explicit) == mixedExplicit, "Mixed Explicit serialization");
    requireSame(ServiceConfiguration::Parse(mixedCompact), value);
    requireSame(ServiceConfiguration::Parse(mixedExplicit), value);
}

void testInvalidArguments() {
    const std::vector<Arguments> invalid{
        {"--lpd-queue", "Queue"}, {"--lpd-queue", "=Printer"}, {"--lpd-queue", "Queue="},
        {"--lpd-queue", "Office=P", "--lpd-queue", "oFFICE=Q"},
        {"--raw-server", ""}, {"--raw-server", ":9100"}, {"--raw-server", "P:"},
        {"--raw-server", "P:0"}, {"--raw-server", "P:9100", "--raw-server", "Q:9100"},
        {"--raw-server", "P:515", "--lpd-queue", "Q=P"},
        {"--listen-address", "256.1.1.1"},
        {"--lpd-queue", "Q=P", "--lpd-port", "1515", "--snmp-ppm"},
        {"--lpd-queue", "two words=P", "--snmp-ppm"},
        {"--lpd-queue", std::string(33, 'q') + "=P", "--snmp-ppm"},
        {"--lpd-queue", "\u6253=P", "--snmp-ppm"},
        {"--log-level", "verbose"}, {"--unknown"}, {"--lpd-disable"},
        {"--service", "install"}, {"--service", "stop"}, {"--silent"}, {"--help"}, {"--version"}
    };
    for (size_t index = 0; index < invalid.size(); ++index) {
        runTest("invalid argument case " + std::to_string(index), [&] {
            for (const bool runnable : {false, true}) {
                requireThrows([&] { ServiceConfiguration::Parse(invalid[index], runnable); },
                    "Invalid configuration accepted (including with listener check disabled)");
            }
        });
    }
    for (const auto* option : {"--raw-server", "--lpd-queue", "--raw-server-min-threads",
        "--max-pending-clients", "--lpd-port", "--lpd-max-control-file-bytes",
        "--lpd-min-threads", "--listen-address", "--log-level"}) {
        requireThrows([&] { ServiceConfiguration::Parse({option}, false); }, "Missing option value accepted");
    }
    for (const auto* option : {"--raw-server-min-threads", "--lpd-min-threads", "--max-pending-clients", "--lpd-port"}) {
        for (const auto* bad : {"", "0", "-1", "+1", "65536", "1x", " 1", "1 ", "18446744073709551616"}) {
            requireThrows([&] { ServiceConfiguration::Parse({option, bad}, false); },
                std::string(option) + " accepted " + bad);
        }
    }
    for (const auto* bad : {"", "0", "-1", "+1", "65536", "1x", " 1", "1 "}) {
        requireThrows([&] { ServiceConfiguration::Parse({"--raw-server", std::string("P:") + bad}, false); },
            "Bad RAW port accepted");
    }
    for (const auto* bad : {"", "0", "-1", "+1", "1x", " 1", "1 ", "18446744073709551616"}) {
        requireThrows([&] { ServiceConfiguration::Parse({"--lpd-max-control-file-bytes", bad}, false); },
            "Bad control byte count accepted");
    }
    const auto listenerless = ServiceConfiguration::Parse({"--snmp-ppm", "--lpd-port", "1515"}, false);
    require(listenerless.snmpPpm && listenerless.lpdPort == 1515, "Nondefault port allowed without LPD mappings");
    requireThrows([] { ServiceConfiguration::Parse({"--snmp-ppm"}); }, "SNMP PPM is not a runnable print listener");
    ServiceConfiguration::Parse({"--lpd-queue", std::string(32, 'q') + "=P", "--snmp-ppm"});
    ServiceConfiguration::Parse({"--lpd-queue", "two words=P"});
}

void testNumericBoundariesAndValidate() {
    for (const auto* limit : {"1", "65535"}) {
        const auto parsed = ServiceConfiguration::Parse({"--raw-server", std::string("P:") + limit,
            "--raw-server-min-threads", limit, "--lpd-min-threads", limit,
            "--max-pending-clients", limit, "--lpd-port", limit});
        const auto number = std::stoi(limit);
        require(parsed.rawServers[0].port == number && parsed.lpdPort == number &&
            parsed.rawServerMinThreads == number && parsed.lpdMinThreads == number &&
            parsed.maxPendingClients == number, "Numeric boundary changed");
    }
    const auto maximum = (std::numeric_limits<size_t>::max)();
    for (const auto limit : {size_t{1}, maximum}) {
        const auto parsed = ServiceConfiguration::Parse({"--lpd-max-control-file-bytes", std::to_string(limit)}, false);
        require(parsed.lpdMaxControlFileBytes == limit, "size_t control limit boundary");
        parsed.Validate(false);
    }
    if constexpr (sizeof(size_t) < sizeof(uint64_t)) {
        requireThrows([] { ServiceConfiguration::Parse({"--lpd-max-control-file-bytes", "4294967296"}, false); },
            "x86 must reject a control limit above SIZE_MAX");
    } else {
        require(ServiceConfiguration::Parse({"--lpd-max-control-file-bytes", "4294967296"}, false)
            .lpdMaxControlFileBytes == uint64_t{4294967296}, "x64 control limit must not truncate");
    }

    const std::vector<void (*)(ServiceConfiguration&)> mutations{
        [](auto& c) { c.rawServerMinThreads = 0; }, [](auto& c) { c.rawServerMinThreads = -1; },
        [](auto& c) { c.lpdMinThreads = 0; }, [](auto& c) { c.maxPendingClients = -1; },
        [](auto& c) { c.lpdPort = 0; }, [](auto& c) { c.lpdMaxControlFileBytes = 0; },
        [](auto& c) { c.rawServers[0].port = 0; }, [](auto& c) { c.rawServers[0].printerName.clear(); },
        [](auto& c) { c.lpdQueues = {{"", "P"}}; }, [](auto& c) { c.lpdQueues = {{"Q", ""}}; },
        [](auto& c) { c.lpdQueues = {{"Q=R", "P"}}; }
    };
    for (const auto mutate : mutations) {
        ServiceConfiguration value;
        value.rawServers = {{"P", 9100}};
        mutate(value);
        requireThrows([&] { value.Validate(false); }, "Validate(false) skipped a non-listener invariant");
    }
    for (const auto field : {&ServiceConfiguration::rawServerMinThreads,
        &ServiceConfiguration::lpdMinThreads, &ServiceConfiguration::maxPendingClients}) {
        for (const auto limit : {65536, (std::numeric_limits<int>::max)()}) {
            ServiceConfiguration value;
            value.rawServers = {{"P", 9100}};
            value.*field = limit;
            requireThrows([&] { value.Validate(); }, "Validate accepted an integer above UINT16_MAX");
            requireThrows([&] { value.Validate(false); }, "Validate(false) skipped the integer upper bound");
            requireThrows([&] { value.ToArguments(); }, "Compact serialization accepted an oversized integer");
            requireThrows([&] { value.ToArguments(Format::Explicit); },
                "Explicit serialization accepted an oversized integer");
        }
    }
    ServiceConfiguration upper;
    upper.rawServers = {{"P", 65535}};
    upper.rawServerMinThreads = upper.lpdMinThreads = upper.maxPendingClients = 65535;
    upper.Validate();
    requireSame(ServiceConfiguration::Parse(upper.ToArguments()), upper);
    requireSame(ServiceConfiguration::Parse(upper.ToArguments(Format::Explicit)), upper);
}

void testIpv4AndLogLevels() {
    for (const auto* valid : {"0.0.0.0", "255.255.255.255", "127.0.0.1", "1.2.3.4", "001.002.003.004"}) {
        require(ServiceConfiguration::IsIpv4Address(valid), std::string("Valid IPv4 rejected: ") + valid);
    }
    for (const auto& invalid : std::vector<std::string>{"", "1.2.3", "1.2.3.4.5", ".1.2.3", "1..2.3", "1.2.3.",
        "256.1.1.1", "1.2.3.256", "0000.1.1.1", "-1.2.3.4", "+1.2.3.4", " 1.2.3.4", "1.2.3.4 ",
        "1.2.3.\t", "a.2.3.4", "::1", std::string("1.2.3.4\0", 8)}) {
        require(!ServiceConfiguration::IsIpv4Address(invalid), "Invalid IPv4 accepted");
    }
    const std::vector<std::pair<ServiceLogLevel, std::string>> levels{
        {ServiceLogLevel::Trace, "trace"}, {ServiceLogLevel::Debug, "debug"}, {ServiceLogLevel::Info, "info"},
        {ServiceLogLevel::Warn, "warn"}, {ServiceLogLevel::Error, "error"},
        {ServiceLogLevel::Critical, "critical"}, {ServiceLogLevel::Off, "off"}
    };
    for (size_t index = 0; index < levels.size(); ++index) {
        const auto& [level, name] = levels[index];
        auto uppercase = name;
        for (auto& ch : uppercase) ch = static_cast<char>(ch - 'a' + 'A');
        require(ServiceConfiguration::LogLevelToString(level) == name, "Log level spelling");
        require(ServiceConfiguration::ParseLogLevel(name) == level &&
            ServiceConfiguration::ParseLogLevel(uppercase) == level &&
            ServiceConfiguration::ParseLogLevel(std::to_string(index)) == level, "Log level alias parsing");
        ServiceConfiguration config;
        config.rawServers = {{"P", 9100}};
        config.logLevel = level;
        const auto args = config.ToArguments(Format::Explicit);
        require(args[args.size() - 2] == "--log-level" && args.back() == name, "Serialized log level text");
    }
    require(ServiceConfiguration::ParseLogLevel("WaRnInG") == ServiceLogLevel::Warn, "warning alias");
    for (const auto* invalid : {"", "7", "-1", " info", "info ", "verbose", "00"}) {
        requireThrows([&] { ServiceConfiguration::ParseLogLevel(invalid); }, "Invalid log level accepted");
    }
    requireThrows([] { ServiceConfiguration::LogLevelToString(static_cast<ServiceLogLevel>(99)); }, "Unknown enum accepted");
}

void testQueueResolution() {
    const std::vector<LpdQueueMapping> mappings{{"Sales", "Office Printer"}, {"Reports", "Other"}};
    require(ResolveLpdQueuePrinter(mappings, "sAlEs") == std::optional<std::string>{"Office Printer"}, "ASCII queue lookup");
    require(ResolveLpdQueuePrinter(mappings, "REPORTS") == std::optional<std::string>{"Other"}, "Second queue lookup");
    for (const auto* absent : {"", "Sale", "Sales ", " Sales", "Office Printer", "unknown"}) {
        require(!ResolveLpdQueuePrinter(mappings, absent), "Queue lookup must be exact, without printer fallback");
    }
    require(!ResolveLpdQueuePrinter({}, "Sales"), "Empty mapping lookup");
}

void testWindowsQuoting() {
    const std::vector<std::pair<std::wstring, std::wstring>> examples{
        {L"plain", L"plain"}, {L"", L"\"\""}, {L"two words", L"\"two words\""},
        {L"a\tb", L"\"a\tb\""}, {L"a\"b", L"\"a\\\"b\""},
        {L"path\\", L"path\\"}, {L"two words\\", L"\"two words\\\\\""},
        {L"\u6253\u5370\u673A", L"\u6253\u5370\u673A"},
        {L"Office \u6253\u5370\u673A", L"\"Office \u6253\u5370\u673A\""}
    };
    for (const auto& [input, expected] : examples) {
        require(QuoteWindowsCommandLineArgument(input) == expected, "Exact Windows argument quoting");
    }
    for (size_t count = 0; count < 5; ++count) {
        const auto input = L"a" + std::wstring(count, L'\\') + L"\"b";
        const auto expected = L"\"a" + std::wstring(count * 2 + 1, L'\\') + L"\"b\"";
        require(QuoteWindowsCommandLineArgument(input) == expected, "Backslashes immediately before a quote");
    }
    require(BuildWindowsCommandLine({}).empty(), "Empty argument vector");
    require(BuildWindowsCommandLine({L"tool.exe", L"", L"two words", L"tail\\"}) ==
        L"tool.exe \"\" \"two words\" tail\\", "Exact command line spacing");

    // argv[0] has special Windows parsing rules; use a normal synthetic name.
    std::vector<std::wstring> arguments{L"test tool.exe"};
    for (const auto& [input, expected] : examples) arguments.push_back(input);
    arguments.push_back(L"a\\\\\"quoted\\");
    const auto command = BuildWindowsCommandLine(arguments);
    int count = 0;
    auto* parsed = CommandLineToArgvW(command.c_str(), &count);
    require(parsed != nullptr, "CommandLineToArgvW failed");
    struct LocalArgvDeleter {
        void operator()(wchar_t** value) const { LocalFree(value); }
    };
    const std::unique_ptr<wchar_t*, LocalArgvDeleter> owner(parsed);
    require(count == static_cast<int>(arguments.size()), "Windows round-trip argument count");
    for (int index = 0; index < count; ++index) {
        require(arguments[static_cast<size_t>(index)] == parsed[index], "Windows round-trip argument bytes");
    }
}

} // namespace

int main() {
    try {
        runTest("defaults", testDefaults);
        runTest("full parse and round trips", testFullParseAndRoundTrips);
        runTest("serialization defaults and flags", testSerializationDefaultsAndFlags);
        runTest("invalid arguments", testInvalidArguments);
        runTest("numeric boundaries and Validate", testNumericBoundariesAndValidate);
        runTest("IPv4 and log levels", testIpv4AndLogLevels);
        runTest("queue resolution", testQueueResolution);
        runTest("Windows quoting", testWindowsQuoting);
    } catch (const std::exception& error) {
        std::cerr << "Configuration tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
