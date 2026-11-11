#include <PpmMib.h>
#include <PrinterMetadata.h>
#include <ServiceConfiguration.hpp>

#include "TestSupport.h"

#include <Windows.h>
#include <winspool.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace {

const snmp::Oid kPpmGeneral{1, 3, 6, 1, 4, 1, 2699, 1, 2, 1, 1};
const snmp::Oid kPpmPrinterEntry{1, 3, 6, 1, 4, 1, 2699, 1, 2, 1, 2, 1, 1};
const snmp::Oid kPpmPortEntry{1, 3, 6, 1, 4, 1, 2699, 1, 2, 1, 3, 1, 1};
const snmp::Oid kDeviceIndex{1, 3, 6, 1, 2, 1, 25, 3, 2, 1, 1};
const snmp::Oid kDeviceType{1, 3, 6, 1, 2, 1, 25, 3, 2, 1, 2};
const snmp::Oid kDeviceStatus{1, 3, 6, 1, 2, 1, 25, 3, 2, 1, 5};
const snmp::Oid kPrinterStatus{1, 3, 6, 1, 2, 1, 25, 3, 5, 1, 1};
const snmp::Oid kPrinterErrors{1, 3, 6, 1, 2, 1, 25, 3, 5, 1, 2};

snmp::Oid indexed(snmp::Oid prefix, std::initializer_list<std::uint32_t> suffix) {
    prefix.insert(prefix.end(), suffix.begin(), suffix.end());
    return prefix;
}

void expectValue(const snmp::Mib& mib, const snmp::Oid& oid, std::uint8_t tag, const std::string& payload) {
    std::string label;
    for (const auto arc : oid) label += "." + std::to_string(arc);
    const auto found = mib.values.find(oid);
    require(found != mib.values.end(), "Missing MIB instance " + label);
    require(found->second.tag == tag && found->second.payload == payload, "MIB value " + label);
}

void requireSameMib(const snmp::Mib& actual, const snmp::Mib& expected) {
    require(actual.objects == expected.objects && actual.values.size() == expected.values.size(), "MIB shape/order invariance");
    for (const auto& [oid, value] : expected.values) expectValue(actual, oid, value.tag, value.payload);
}

ServiceConfiguration configuration() {
    ServiceConfiguration result;
    // ASCII input makes CP_ACP decoding deterministic on every Windows host.
    result.printerMap = {
        {"Zebra", "zQueue", RawServerConfiguration{"0.0.0.0", 9200}},
        {"ALPHA", "aQueue", RawServerConfiguration{"0.0.0.0", 9102}},
        {"alpha", "only", std::nullopt},
        {"Beta", "betaQueue", std::nullopt}
    };
    return result;
}

void testDirectoryIndexesAndAllColumns() {
    const auto config = configuration();
    const auto original = config;
    const auto directory = PpmMib::BuildDirectory(config);
    require(config.ppmDisabled == original.ppmDisabled &&
        config.lpdServer.listenAddress == original.lpdServer.listenAddress &&
        config.lpdServer.port == original.lpdServer.port, "BuildDirectory must not mutate configuration");
    require(directory.printers.size() == 3, "Case-insensitive backend printer grouping");
    const std::array<std::wstring, 3> names{L"ALPHA", L"Beta", L"Zebra"};
    const std::array<std::string, 3> captions{"ALPHA", "Beta", "Zebra"};
    const std::array<std::uint8_t, 3> portCounts{3, 1, 2};
    for (size_t index = 0; index < names.size(); ++index) {
        require(directory.printers[index].index == index + 1 && directory.printers[index].queueName == names[index],
            "Contiguous, case-insensitive sorted indexes and deterministic display spelling");
    }
    const auto& mib = directory.baseMib;
    // Four scalars, twelve values per printer, and eight values per port:
    // 4 + 3 * 12 + 6 * 8 = 88 values; 4 + 12 + 8 = 24 registered objects.
    require(mib.values.size() == 88 && mib.objects.size() == 24, "Directory MIB shape");
    expectValue(mib, {1, 3, 6, 1, 2, 1, 1, 1, 0}, 4, "PrintHub PPM print server");
    expectValue(mib, indexed(kPpmGeneral, {1, 0}), 4, "");
    expectValue(mib, indexed(kPpmGeneral, {2, 0}), 0x42, bytes({3}));
    expectValue(mib, indexed(kPpmGeneral, {3, 0}), 0x42, bytes({6}));
    for (std::uint32_t column = 2; column <= 8; ++column) {
        require(mib.objects.contains(indexed(kPpmPrinterEntry, {column})), "Registered printer object");
    }
    for (std::uint32_t column = 2; column <= 9; ++column) {
        require(mib.objects.contains(indexed(kPpmPortEntry, {column})), "Registered port object");
    }
    for (const auto& object : {kDeviceIndex, kDeviceType, kDeviceStatus, kPrinterStatus, kPrinterErrors}) {
        require(mib.objects.contains(object), "Registered host-resources object");
    }
    for (std::uint32_t index = 1; index <= 3; ++index) {
        expectValue(mib, indexed(kPpmPrinterEntry, {2, index}), 4, captions[index - 1]);
        expectValue(mib, indexed(kPpmPrinterEntry, {3, index}), 4, "");
        expectValue(mib, indexed(kPpmPrinterEntry, {4, index}), 0x42, bytes({portCounts[index - 1]}));
        expectValue(mib, indexed(kPpmPrinterEntry, {5, index}), 2, bytes({1}));
        expectValue(mib, indexed(kPpmPrinterEntry, {6, index}), 2, bytes({static_cast<std::uint8_t>(index)}));
        expectValue(mib, indexed(kPpmPrinterEntry, {7, index}), 4, "public");
        expectValue(mib, indexed(kPpmPrinterEntry, {8, index}), 2, bytes({2}));
        expectValue(mib, indexed(kDeviceIndex, {index}), 2, bytes({static_cast<std::uint8_t>(index)}));
        expectValue(mib, indexed(kDeviceType, {index}), 6, bytes({0x2B, 6, 1, 2, 1, 25, 3, 1, 5}));
        expectValue(mib, indexed(kDeviceStatus, {index}), 2, bytes({1}));
        expectValue(mib, indexed(kPrinterStatus, {index}), 2, bytes({2}));
        expectValue(mib, indexed(kPrinterErrors, {index}), 4, bytes({0, 0}));
    }
    struct Port {
        std::uint32_t printer;
        std::uint32_t index;
        std::string caption;
        std::string queue;
        bool raw;
        std::string portBytes;
    };
    const std::vector<Port> ports{
        {1, 1, "ALPHA - RAW TCP 9102", "", true, bytes({0x23, 0x8E})},
        {1, 2, "ALPHA - LPR aQueue (TCP 515)", "aQueue", false, bytes({2, 3})},
        {1, 3, "ALPHA - LPR only (TCP 515)", "only", false, bytes({2, 3})},
        {2, 1, "Beta - LPR betaQueue (TCP 515)", "betaQueue", false, bytes({2, 3})},
        {3, 1, "Zebra - RAW TCP 9200", "", true, bytes({0x23, 0xF0})},
        {3, 2, "Zebra - LPR zQueue (TCP 515)", "zQueue", false, bytes({2, 3})}
    };
    for (const auto& port : ports) {
        expectValue(mib, indexed(kPpmPortEntry, {2, port.printer, port.index}), 2, bytes({1}));
        expectValue(mib, indexed(kPpmPortEntry, {3, port.printer, port.index}), 4, port.caption);
        expectValue(mib, indexed(kPpmPortEntry, {4, port.printer, port.index}), 4, port.queue);
        expectValue(mib, indexed(kPpmPortEntry, {5, port.printer, port.index}), 2,
            bytes({static_cast<std::uint8_t>(port.raw ? 11 : 8)}));
        expectValue(mib, indexed(kPpmPortEntry, {6, port.printer, port.index}), 2, port.portBytes);
        expectValue(mib, indexed(kPpmPortEntry, {7, port.printer, port.index}), 2, bytes({1}));
        expectValue(mib, indexed(kPpmPortEntry, {8, port.printer, port.index}), 2, bytes({0}));
        expectValue(mib, indexed(kPpmPortEntry, {9, port.printer, port.index}), 2,
            bytes({static_cast<std::uint8_t>(port.raw ? 2 : 1)}));
    }
    auto reordered = config;
    std::reverse(reordered.printerMap.begin(), reordered.printerMap.end());
    const auto second = PpmMib::BuildDirectory(reordered);
    requireSameMib(second.baseMib, mib);
    require(second.printers.size() == directory.printers.size(), "Reordered printer count");
    for (size_t index = 0; index < directory.printers.size(); ++index) {
        require(second.printers[index].index == directory.printers[index].index &&
            second.printers[index].queueName == directory.printers[index].queueName, "Reordered printer identity");
    }

    auto disabled = config;
    disabled.ppmDisabled = true;
    const auto originalDisabled = disabled;
    requireSameMib(PpmMib::BuildDirectory(disabled).baseMib, mib);
    require(disabled.ppmDisabled == originalDisabled.ppmDisabled &&
        disabled.lpdServer.listenAddress == originalDisabled.lpdServer.listenAddress &&
        disabled.lpdServer.port == originalDisabled.lpdServer.port,
        "Building PPM for a disabled configuration must not mutate the caller");
}

void testPortOrdering() {
    auto config = configuration();
    config.printerMap[2].rawServer = RawServerConfiguration{"0.0.0.0", 9100};
    config.printerMap[2].lprQueue = "ZQueue2";
    const auto directory = PpmMib::BuildDirectory(config);
    const auto& mib = directory.baseMib;
    expectValue(mib, indexed(kPpmGeneral, {3, 0}), 0x42, bytes({7}));
    expectValue(mib, indexed(kPpmPrinterEntry, {4, 1}), 0x42, bytes({4}));
    expectValue(mib, indexed(kPpmPortEntry, {3, 1, 1}), 4, "ALPHA - RAW TCP 9100");
    expectValue(mib, indexed(kPpmPortEntry, {3, 1, 2}), 4, "ALPHA - RAW TCP 9102");
    expectValue(mib, indexed(kPpmPortEntry, {3, 1, 3}), 4, "ALPHA - LPR aQueue (TCP 515)");
    expectValue(mib, indexed(kPpmPortEntry, {3, 1, 4}), 4, "ALPHA - LPR ZQueue2 (TCP 515)");
    std::reverse(config.printerMap.begin(), config.printerMap.end());
    requireSameMib(PpmMib::BuildDirectory(config).baseMib, mib);
}

void testCaptionsAndInvalidConfiguration() {
    ServiceConfiguration longNames;
    const auto prefix = std::string(70, 'A');
    longNames.printerMap = {
        {prefix + "-two", "two", RawServerConfiguration{"0.0.0.0", 9101}},
        {prefix + "-one", "one", RawServerConfiguration{"0.0.0.0", 9100}}
    };
    const auto directory = PpmMib::BuildDirectory(longNames);
    require(directory.printers.size() == 2 && directory.printers[0].queueName == std::wstring(70, L'A') + L"-one",
        "Directory retains full backend names");
    const auto first = std::string(59, 'A') + " [1]";
    const auto second = std::string(59, 'A') + " [2]";
    expectValue(directory.baseMib, indexed(kPpmPrinterEntry, {2, 1}), 4, first);
    expectValue(directory.baseMib, indexed(kPpmPrinterEntry, {2, 2}), 4, second);
    expectValue(directory.baseMib, indexed(kPpmPortEntry, {3, 1, 1}), 4, first + " - RAW TCP 9100");
    expectValue(directory.baseMib, indexed(kPpmPortEntry, {3, 1, 2}), 4, first + " - LPR one (TCP 515)");
    expectValue(directory.baseMib, indexed(kPpmPortEntry, {3, 2, 1}), 4, second + " - RAW TCP 9101");
    expectValue(directory.baseMib, indexed(kPpmPortEntry, {3, 2, 2}), 4, second + " - LPR two (TCP 515)");
    ServiceConfiguration single;
    single.printerMap = {{std::string(64, 'B'), "single", std::nullopt}};
    const auto singleDirectory = PpmMib::BuildDirectory(single);
    expectValue(singleDirectory.baseMib, indexed(kPpmPrinterEntry, {2, 1}), 4, std::string(63, 'B'));
    expectValue(singleDirectory.baseMib, indexed(kPpmPortEntry, {3, 1, 1}), 4,
        std::string(63, 'B') + " - LPR single (TCP 515)");

    const auto empty = PpmMib::BuildDirectory(ServiceConfiguration{});
    require(empty.printers.empty(), "Empty printerMap has no printers");
    require(empty.baseMib.values.size() == 4 && empty.baseMib.objects == directory.baseMib.objects,
        "Empty directory retains all scalars and registered objects without table rows");
    expectValue(empty.baseMib, indexed(kPpmGeneral, {1, 0}), 4, "");
    expectValue(empty.baseMib, indexed(kPpmGeneral, {2, 0}), 0x42, bytes({0}));
    expectValue(empty.baseMib, indexed(kPpmGeneral, {3, 0}), 0x42, bytes({0}));
    const std::vector<void (*)(ServiceConfiguration&)> mutations{
        [](auto& c) { c.lpdServer.port = 1515; },
        [](auto& c) { c.printerMap[0].lprQueue = "two words"; },
        [](auto& c) { c.printerMap[0].lprQueue.assign(33, 'x'); },
        [](auto& c) { c.printerMap[0].name = std::string("A\0B", 3); },
        [](auto& c) { c.printerMap[0].rawServer->port = 0; },
        [](auto& c) { c.lpdServer.listenAddress = "999.0.0.1"; },
        [](auto& c) {
            c.lpdServer.listenAddress = "0.0.0.0";
            c.printerMap[0].rawServer->listenAddress = "127.0.0.1";
        }
    };
    for (const auto mutate : mutations) {
        for (const auto disabled : {false, true}) {
            auto config = configuration();
            config.ppmDisabled = disabled;
            mutate(config);
            const auto original = config;
            requireThrows([&] { PpmMib::BuildDirectory(config); }, "BuildDirectory must enforce PPM-specific validation");
            require(config.ppmDisabled == original.ppmDisabled &&
                config.lpdServer.listenAddress == original.lpdServer.listenAddress &&
                config.lpdServer.port == original.lpdServer.port, "Failed validation must not mutate configuration");
        }
    }
}

void testMetadataOverlayAndStaleness() {
    using namespace std::chrono;
    const auto now = ppm::Clock::time_point{} + seconds(100);
    const auto directory = PpmMib::BuildDirectory(configuration());
    const auto original = directory.baseMib;
    const std::string deviceId = "MFG:Example;MDL:Laser 1;CMD:PCL,POSTSCRIPT;";
    const std::array<ppm::StatusSample, 3> statuses{{
        {ppm::QueryOutcome::Success, PRINTER_STATUS_PRINTING, now},
        {ppm::QueryOutcome::Success, PRINTER_STATUS_OFFLINE | PRINTER_STATUS_PAPER_OUT |
            PRINTER_STATUS_TONER_LOW | PRINTER_STATUS_OUTPUT_BIN_FULL, now - seconds(30)},
        {ppm::QueryOutcome::Failed, PRINTER_STATUS_PRINTING, now}
    }};
    const std::array<ppm::IdentitySample, 3> identities{{
        {ppm::QueryOutcome::Success, deviceId},
        {ppm::QueryOutcome::Success, "MFG:Missing model;"},
        {ppm::QueryOutcome::NotFound, deviceId}
    }};
    const auto updated = PpmMib::WithMetadata(directory, statuses, identities, now);
    expectValue(updated, indexed(kDeviceStatus, {1}), 2, bytes({2}));
    expectValue(updated, indexed(kPrinterStatus, {1}), 2, bytes({4}));
    expectValue(updated, indexed(kPrinterErrors, {1}), 4, bytes({0, 0}));
    expectValue(updated, indexed(kPpmPrinterEntry, {3, 1}), 4, deviceId);
    expectValue(updated, indexed(kDeviceStatus, {2}), 2, bytes({5}));
    expectValue(updated, indexed(kPrinterStatus, {2}), 2, bytes({1}));
    expectValue(updated, indexed(kPrinterErrors, {2}), 4, bytes({0x62, 0x08}));
    expectValue(updated, indexed(kPpmPrinterEntry, {3, 2}), 4, "");
    expectValue(updated, indexed(kDeviceStatus, {3}), 2, bytes({1}));
    expectValue(updated, indexed(kPrinterStatus, {3}), 2, bytes({2}));
    expectValue(updated, indexed(kPrinterErrors, {3}), 4, bytes({0, 0}));
    expectValue(updated, indexed(kPpmPrinterEntry, {3, 3}), 4, "");
    requireSameMib(directory.baseMib, original);
    require(updated.objects == original.objects && updated.values.size() == original.values.size(),
        "Overlay must not alter MIB schema");
    std::set<snmp::Oid> mutableValues;
    for (std::uint32_t index = 1; index <= 3; ++index) {
        mutableValues.insert(indexed(kDeviceStatus, {index}));
        mutableValues.insert(indexed(kPrinterStatus, {index}));
        mutableValues.insert(indexed(kPrinterErrors, {index}));
        mutableValues.insert(indexed(kPpmPrinterEntry, {3, index}));
    }
    for (const auto& [oid, value] : original.values) {
        if (!mutableValues.contains(oid)) expectValue(updated, oid, value.tag, value.payload);
    }
    const auto stale = PpmMib::WithMetadata(directory, statuses, identities, now + seconds(1));
    expectValue(stale, indexed(kDeviceStatus, {2}), 2, bytes({1}));
    expectValue(stale, indexed(kPrinterStatus, {2}), 2, bytes({2}));
    expectValue(stale, indexed(kPrinterErrors, {2}), 4, bytes({0, 0}));
    expectValue(stale, indexed(kPpmPrinterEntry, {3, 1}), 4, deviceId);
    requireThrows([&] {
        PpmMib::WithMetadata(directory, std::span<const ppm::StatusSample>(statuses).first(2), identities, now);
    }, "Status sample count mismatch");
    requireThrows([&] {
        PpmMib::WithMetadata(directory, statuses, std::span<const ppm::IdentitySample>(identities).first(2), now);
    }, "Identity sample count mismatch");
    const PpmDirectory empty;
    require(PpmMib::WithMetadata(empty, {}, {}, now).values.empty(), "Empty overlay is well-defined");
    const auto emptyDirectory = PpmMib::BuildDirectory(ServiceConfiguration{});
    requireSameMib(PpmMib::WithMetadata(emptyDirectory, {}, {}, now), emptyDirectory.baseMib);
}

void testStatusMapping() {
    using namespace std::chrono;
    const auto now = ppm::Clock::time_point{} + seconds(100);
    struct Example {
        std::uint32_t flags;
        std::int32_t device;
        std::int32_t printer;
        std::string errors;
    };
    const std::vector<Example> examples{
        {0, 2, 3, bytes({0, 0})},
        {PRINTER_STATUS_TONER_LOW, 3, 3, bytes({0x20, 0})},
        {PRINTER_STATUS_PAPER_OUT, 5, 1, bytes({0x40, 0})},
        {PRINTER_STATUS_NO_TONER, 5, 1, bytes({0x10, 0})},
        {PRINTER_STATUS_DOOR_OPEN, 5, 1, bytes({0x08, 0})},
        {PRINTER_STATUS_PAPER_JAM, 5, 1, bytes({0x04, 0})},
        {PRINTER_STATUS_OFFLINE, 5, 1, bytes({0x02, 0})},
        {PRINTER_STATUS_OUTPUT_BIN_FULL, 5, 1, bytes({0, 0x08})},
        {PRINTER_STATUS_USER_INTERVENTION, 3, 3, bytes({0x01, 0})},
        {PRINTER_STATUS_ERROR, 3, 3, bytes({0x01, 0})},
        {PRINTER_STATUS_PAPER_PROBLEM, 3, 3, bytes({0x01, 0})},
        {PRINTER_STATUS_OUT_OF_MEMORY, 3, 3, bytes({0x01, 0})},
        {PRINTER_STATUS_PAGE_PUNT, 3, 3, bytes({0x01, 0})},
        {PRINTER_STATUS_MANUAL_FEED, 3, 3, bytes({0, 0})},
        {PRINTER_STATUS_PAUSED, 5, 1, bytes({0, 0})},
        {PRINTER_STATUS_NOT_AVAILABLE, 5, 1, bytes({0, 0})},
        {PRINTER_STATUS_PENDING_DELETION, 5, 1, bytes({0, 0})},
        {PRINTER_STATUS_WARMING_UP | PRINTER_STATUS_PRINTING, 2, 5, bytes({0, 0})},
        {PRINTER_STATUS_INITIALIZING, 2, 5, bytes({0, 0})},
        {PRINTER_STATUS_PRINTING | PRINTER_STATUS_TONER_LOW, 3, 4, bytes({0x20, 0})},
        {PRINTER_STATUS_BUSY, 2, 1, bytes({0, 0})},
        {PRINTER_STATUS_IO_ACTIVE, 2, 1, bytes({0, 0})},
        {PRINTER_STATUS_PROCESSING, 2, 1, bytes({0, 0})},
        {PRINTER_STATUS_WAITING | PRINTER_STATUS_POWER_SAVE, 2, 3, bytes({0, 0})},
        {PRINTER_STATUS_OFFLINE | PRINTER_STATUS_PRINTING, 5, 1, bytes({0x02, 0})},
        {PRINTER_STATUS_SERVER_UNKNOWN | PRINTER_STATUS_PAPER_OUT, 1, 2, bytes({0, 0})},
        {0x80000000U, 1, 2, bytes({0, 0})}
    };
    for (const auto& example : examples) {
        const auto actual = ppm::MapStatus({ppm::QueryOutcome::Success, example.flags, now}, now);
        require(actual.deviceStatus == example.device && actual.printerStatus == example.printer &&
            actual.detectedErrors == example.errors, "Status flags " + std::to_string(example.flags));
    }
    for (const auto outcome : {ppm::QueryOutcome::Failed, ppm::QueryOutcome::NotFound, ppm::QueryOutcome::Unsupported}) {
        const auto actual = ppm::MapStatus({outcome, PRINTER_STATUS_OFFLINE, now}, now);
        require(actual.deviceStatus == 1 && actual.printerStatus == 2 && actual.detectedErrors == bytes({0, 0}),
            "Unavailable status must be unknown, not a fabricated error");
    }
    const auto fresh = ppm::MapStatus({ppm::QueryOutcome::Success, 0, now - seconds(30)}, now);
    const auto stale = ppm::MapStatus({ppm::QueryOutcome::Success, 0, now - seconds(30) - milliseconds(1)}, now);
    require(fresh.deviceStatus == 2 && fresh.printerStatus == 3 &&
        stale.deviceStatus == 1 && stale.printerStatus == 2, "Status age boundary");
}

void testDeviceIdValidation() {
    const auto longest = std::string("MFG:A;MDL:") + std::string(1012, 'x') + ";";
    require(longest.size() == 1023, "Device ID maximum-length fixture");
    for (const auto& valid : std::vector<std::string>{"MFG:A;MDL:B;", "MANUFACTURER:A;MODEL:B;",
        " \tMDL: B ; MFG: A ; CMD:PCL, POSTSCRIPT; \r\n", longest}) {
        require(ppm::ValidateDeviceId(valid), "Valid device ID rejected");
    }
    for (const auto& invalid : std::vector<std::string>{"", "MFG:A;", "MDL:B;", "mfg:A;mdl:B;",
        "MFG:A;MDL:B", "MFG:;MDL:B;", "MFG:A;MDL: ;", "MFG:A;MDL:B;;",
        "MFG:A;MDL:B;CMD:PCL,;", "MFG:A;MDL:B;CMD:,PCL;", "MFG:A;MDL:B;CMD:PCL,,PS;",
        "MFG:A;MDL:B;CMD:PCL:PS;", "MFG:A;MDL:B;C,MD:PCL;", "MFG:A;MDL:B;:x;",
        std::string("MFG:A;MDL:B;\0", 13), std::string("MFG:A;MDL:") + bytes({0x80}) + ";", longest + " "}) {
        require(!ppm::ValidateDeviceId(invalid), "Invalid device ID accepted");
    }
}

} // namespace

int main() {
    try {
        runTest("directory indexes and all columns", testDirectoryIndexesAndAllColumns);
        runTest("port ordering", testPortOrdering);
        runTest("captions and invalid configuration", testCaptionsAndInvalidConfiguration);
        runTest("metadata overlay and staleness", testMetadataOverlayAndStaleness);
        runTest("status mapping", testStatusMapping);
        runTest("device ID validation", testDeviceIdValidation);
    } catch (const std::exception& error) {
        std::cerr << "PPM MIB tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
