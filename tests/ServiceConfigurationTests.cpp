#include <ServiceConfiguration.hpp>
#include <Ipv4.h>

#include "TestSupport.h"

#include <limits>
#include <vector>

namespace {

ServiceConfiguration mappedConfiguration() {
    ServiceConfiguration value;
    value.printerMap = {{"Printer", "Queue", std::nullopt}};
    return value;
}

void testDefaults() {
    const ServiceConfiguration value;
    require(value.logLevel == ServiceLogLevel::Info, "Default log level");
    require(value.maxPendingClients == 256 && ServiceConfiguration::kDefaultMaxPendingClients == 256,
        "Default pending clients");
    require(value.lpdServer.listenAddress == "0.0.0.0" && value.lpdServer.port == 515 &&
        ServiceConfiguration::kDefaultLpdPort == 515, "Default LPD endpoint");
    require(value.rawServerMinThreads == 1 && value.lpdMinThreads == 1 &&
        ServiceConfiguration::kDefaultMinThreads == 1, "Default threads");
    require(value.printerMap.empty(), "Default mappings");
    require(!value.ppmDisabled, "PPM must be enabled by default");
    require(ServiceConfiguration::kDefaultLpdMaxControlFileBytes == 4 * 1024 * 1024,
        "Fixed LPD control file limit");
    const RawServerConfiguration raw;
    require(raw.listenAddress == "0.0.0.0" && raw.port == 9100, "Default RAW endpoint");
    value.Validate();
}

void testValidConfigurations() {
    auto value = mappedConfiguration();
    value.Validate();
    value.printerMap[0].rawServer.emplace();
    value.Validate();

    // One printer can have several queues and RAW endpoints.
    value.ppmDisabled = true;
    value.printerMap = {
        {"Printer", "Queue1", RawServerConfiguration{"192.168.1.10", 9100}},
        {"Printer", "Queue2", RawServerConfiguration{"192.168.1.11", 9100}},
    };
    value.Validate();
    value.lpdServer = {"192.168.1.12", 515};
    value.printerMap[0].rawServer->port = 515;
    value.printerMap[1].rawServer->port = 515;
    value.Validate();
    value.ppmDisabled = false;
    requireThrows([&] { value.Validate(); }, "PPM accepted listeners on different specific addresses");

    ServiceConfiguration empty;
    empty.lpdServer.port = 1515;
    empty.Validate();

    for (const int boundary : {1, 65535}) {
        auto limits = mappedConfiguration();
        limits.ppmDisabled = true;
        limits.rawServerMinThreads = limits.lpdMinThreads = limits.maxPendingClients = boundary;
        limits.lpdServer.port = static_cast<uint16_t>(boundary);
        limits.lpdServer.listenAddress = "127.0.0.1";
        limits.printerMap[0].rawServer = RawServerConfiguration{"127.0.0.2", static_cast<uint16_t>(boundary)};
        limits.Validate();
    }
}

void testInvalidValues() {
    for (const auto field : {&ServiceConfiguration::rawServerMinThreads,
        &ServiceConfiguration::lpdMinThreads, &ServiceConfiguration::maxPendingClients}) {
        for (const int invalid : {0, -1, 65536, (std::numeric_limits<int>::max)()}) {
            ServiceConfiguration value;
            value.*field = invalid;
            requireThrows([&] { value.Validate(); }, "Invalid thread/client count accepted");
        }
    }
    for (const auto field : {&PrinterMapping::name, &PrinterMapping::lprQueue}) {
        for (const auto& invalid : {std::string{}, std::string("a\0b", 3)}) {
            auto value = mappedConfiguration();
            value.printerMap[0].*field = invalid;
            requireThrows([&] { value.Validate(); }, "Empty or NUL-containing mapping accepted");
            value.ppmDisabled = true;
            requireThrows([&] { value.Validate(); }, "PPM opt-out bypassed a mapping invariant");
        }
    }
    for (const auto& invalid : std::vector<std::string>{"", "256.1.1.1", "not-an-address",
        "::1", std::string("127.0.0.1\0", 10)}) {
        ServiceConfiguration lpd;
        lpd.lpdServer.listenAddress = invalid;
        requireThrows([&] { lpd.Validate(); }, "Invalid LPD address accepted without mappings");
        auto raw = mappedConfiguration();
        raw.printerMap[0].rawServer = RawServerConfiguration{invalid, 9100};
        requireThrows([&] { raw.Validate(); }, "Invalid RAW address accepted");
    }
    ServiceConfiguration lpd;
    lpd.lpdServer.port = 0;
    requireThrows([&] { lpd.Validate(); }, "Zero LPD port accepted");
    auto raw = mappedConfiguration();
    raw.printerMap[0].rawServer = RawServerConfiguration{"0.0.0.0", 0};
    requireThrows([&] { raw.Validate(); }, "Zero RAW port accepted");

    auto duplicate = mappedConfiguration();
    duplicate.printerMap.push_back({"Other Printer", "qUEUE", std::nullopt});
    requireThrows([&] { duplicate.Validate(); }, "Case-insensitive duplicate queue accepted");
}

void testListenerConflicts() {
    for (const auto& addresses : std::vector<std::pair<std::string, std::string>>{
        {"0.0.0.0", "0.0.0.0"}, {"127.0.0.1", "127.0.0.1"},
        {"001.002.003.004", "1.2.3.4"}, {"000.000.000.000", "0.0.0.0"},
        {"0.0.0.0", "127.0.0.1"}, {"127.0.0.1", "0.0.0.0"}}) {
        ServiceConfiguration raw;
        raw.printerMap = {
            {"Printer", "Queue1", RawServerConfiguration{addresses.first, 9100}},
            {"Printer", "Queue2", RawServerConfiguration{addresses.second, 9100}},
        };
        requireThrows([&] { raw.Validate(); }, "Conflicting RAW listeners accepted");

        auto lpd = mappedConfiguration();
        lpd.lpdServer.listenAddress = addresses.first;
        lpd.printerMap[0].rawServer = RawServerConfiguration{addresses.second, 515};
        requireThrows([&] { lpd.Validate(); }, "Conflicting LPD and RAW listeners accepted");
    }

    auto conflict = mappedConfiguration();
    conflict.printerMap[0].rawServer = RawServerConfiguration{"0.0.0.0", 515};
    try {
        conflict.Validate();
        throw std::runtime_error("Expected LPD/RAW conflict");
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        require(message.find("printerMap[0].rawServer") != std::string::npos &&
            message.find("LPD server") != std::string::npos &&
            message.find("0.0.0.0:515") != std::string::npos, "Conflict diagnostic lacks listener details");
    }
}

void testPpmRules() {
    for (const auto& queue : std::vector<std::string>{"two words", std::string(33, 'q'),
        "Q=R", "Q\tR", std::string("Q\x1f", 2), std::string("Q\x7f", 2),
        std::string(1, static_cast<char>(0x80))}) {
        auto value = mappedConfiguration();
        value.printerMap[0].lprQueue = queue;
        requireThrows([&] { value.Validate(); }, "Invalid PPM queue accepted");
        value.ppmDisabled = true;
        value.lpdServer.port = 1515;
        value.Validate();
    }
    for (const auto* queue : {"Q\rR", "Q\nR", "Q\r\nR"}) {
        auto value = mappedConfiguration();
        value.printerMap[0].lprQueue = queue;
        for (const auto disabled : {false, true}) {
            value.ppmDisabled = disabled;
            requireThrows([&] { value.Validate(); }, "CR/LF in LPR queues must be rejected regardless of PPM");
        }
    }
    auto valid = mappedConfiguration();
    valid.printerMap[0].lprQueue = std::string(32, 'q');
    valid.Validate();
    valid.printerMap[0].lprQueue = "!~";
    valid.Validate();

    auto port = mappedConfiguration();
    port.lpdServer.port = 1515;
    try {
        port.Validate();
        throw std::runtime_error("PPM accepted a nonstandard LPD port");
    } catch (const std::invalid_argument& error) {
        require(std::string(error.what()).find("ppmDisabled=true") != std::string::npos,
            "PPM port error must explain the opt-out");
    }
    port.ppmDisabled = true;
    port.Validate();
}

void testPpmListenAddresses() {
    auto value = mappedConfiguration();
    value.lpdServer.listenAddress = "127.0.0.1";
    value.printerMap = {
        {"Printer", "Queue1", RawServerConfiguration{"127.0.0.1", 9100}},
        {"Printer", "Queue2", RawServerConfiguration{"127.0.0.1", 9101}},
        {"Other Printer", "Queue3", std::nullopt},
    };
    value.Validate();
    value.printerMap[1].rawServer->listenAddress = "127.000.000.001";
    value.Validate();

    value.lpdServer.listenAddress = "0.0.0.0";
    value.printerMap[0].rawServer->listenAddress = "0.0.0.0";
    try {
        value.Validate();
        throw std::runtime_error("PPM accepted wildcard and specific listener addresses");
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        require(message.find("printerMap[1].rawServer.listenAddress") != std::string::npos &&
            message.find("ppmDisabled=true") != std::string::npos,
            "PPM address diagnostic must identify the mismatched field and explain the opt-out");
    }
    value.ppmDisabled = true;
    value.Validate();
}

void testIpv4Addresses() {
    require(ParseIpv4Address("001.002.003.004") == ParseIpv4Address("1.2.3.4"),
        "Leading-zero IPv4 octets must be decimal");
    require(ParseIpv4Address("1.2.3.4") == std::optional<uint32_t>{0x01020304u},
        "IPv4 parser must return the host-order address");
    for (const auto* valid : {"0.0.0.0", "255.255.255.255", "127.0.0.1", "1.2.3.4", "001.002.003.004"}) {
        require(ServiceConfiguration::IsIpv4Address(valid), std::string("Valid IPv4 rejected: ") + valid);
    }
    for (const auto& invalid : std::vector<std::string>{"", "1.2.3", "1.2.3.4.5", ".1.2.3", "1..2.3", "1.2.3.",
        "256.1.1.1", "1.2.3.256", "0000.1.1.1", "-1.2.3.4", "+1.2.3.4", " 1.2.3.4", "1.2.3.4 ",
        "1.2.3.\t", "a.2.3.4", "::1", std::string("1.2.3.4\0", 8)}) {
        require(!ServiceConfiguration::IsIpv4Address(invalid), "Invalid IPv4 accepted");
    }
}

void testLogLevels() {
    const std::vector<std::pair<ServiceLogLevel, std::string>> levels{
        {ServiceLogLevel::Trace, "trace"}, {ServiceLogLevel::Debug, "debug"}, {ServiceLogLevel::Info, "info"},
        {ServiceLogLevel::Warn, "warn"}, {ServiceLogLevel::Error, "error"},
        {ServiceLogLevel::Critical, "critical"}, {ServiceLogLevel::Off, "off"}
    };
    for (const auto& [level, name] : levels) {
        auto uppercase = name;
        for (auto& ch : uppercase) ch = static_cast<char>(ch - 'a' + 'A');
        auto mixed = name;
        mixed.front() = uppercase.front();
        require(ServiceConfiguration::LogLevelToString(level) == name, "Log level spelling");
        require(ServiceConfiguration::ParseLogLevel(name) == level &&
            ServiceConfiguration::ParseLogLevel(uppercase) == level &&
            ServiceConfiguration::ParseLogLevel(mixed) == level, "Case-insensitive log level parsing");
    }
    for (const auto* invalid : {"", "0", "6", "warning", "WaRnInG", "verbose", "7", "-1", " info", "info ", "00"}) {
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

} // namespace

int main() {
    try {
        runTest("defaults", testDefaults);
        runTest("valid configurations", testValidConfigurations);
        runTest("invalid values", testInvalidValues);
        runTest("listener conflicts", testListenerConflicts);
        runTest("PPM rules", testPpmRules);
        runTest("PPM listen addresses", testPpmListenAddresses);
        runTest("IPv4 addresses", testIpv4Addresses);
        runTest("log levels", testLogLevels);
        runTest("queue resolution", testQueueResolution);
    } catch (const std::exception& error) {
        std::cerr << "Configuration tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
