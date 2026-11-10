#include <PpmMib.h>

#include <Windows.h>

#include <algorithm>
#include <climits>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kPrinterGroup = 2;
constexpr uint32_t kPortGroup = 3;
constexpr size_t kNameLimitBytes = 127;
// TCPMON copies ppmPrinterName into PORT_DATA2::sztPortName[64].
// Keep distinguishing captions inside that buffer, including its terminator.
constexpr size_t kWindowsCaptionLimitCodeUnits = 63;
const snmp::Oid kPpmObjects{ 1, 3, 6, 1, 4, 1, 2699, 1, 2, 1 };
const snmp::Oid kHrDeviceIndex{ 1, 3, 6, 1, 2, 1, 25, 3, 2, 1, 1 };
const snmp::Oid kHrDeviceType{ 1, 3, 6, 1, 2, 1, 25, 3, 2, 1, 2 };
const snmp::Oid kHrDeviceStatus{ 1, 3, 6, 1, 2, 1, 25, 3, 2, 1, 5 };
const snmp::Oid kHrPrinterStatus{ 1, 3, 6, 1, 2, 1, 25, 3, 5, 1, 1 };
const snmp::Oid kHrPrinterErrors{ 1, 3, 6, 1, 2, 1, 25, 3, 5, 1, 2 };
const snmp::Oid kHrDevicePrinter{ 1, 3, 6, 1, 2, 1, 25, 3, 1, 5 };

std::wstring decodePrinterName(const std::string& text) {
    if (text.empty() || text.size() > INT_MAX || text.find('\0') != std::string::npos) {
        throw std::invalid_argument("PPM printer name must be nonempty without embedded NUL");
    }
    const auto length = static_cast<int>(text.size());
    // ServiceConfiguration uses the process ANSI code page, matching argv,
    // the existing manager and OpenPrinterA. Only the SNMP representation
    // is UTF-8; requiring UTF-8 input would break existing localized queues.
    const auto size = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (size == 0) {
        throw std::invalid_argument("PPM printer name is not valid in the Windows ANSI code page");
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), size) != size) {
        throw std::invalid_argument("Could not decode PPM printer name using the Windows ANSI code page");
    }
    return result;
}

std::string encodeUtf8(const std::wstring& text) {
    const auto length = static_cast<int>(text.size());
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (size == 0) throw std::invalid_argument("Could not encode PPM printer name as UTF-8");
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), size, nullptr, nullptr) != size) {
        throw std::invalid_argument("Could not encode PPM printer name as UTF-8");
    }
    return result;
}

int compareOrdinal(const std::wstring& left, const std::wstring& right, bool ignoreCase) {
    const auto result = CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), ignoreCase ? TRUE : FALSE);
    if (result == 0) {
        throw std::runtime_error("Could not compare PPM printer names");
    }
    return result - CSTR_EQUAL;
}

struct OrdinalIgnoreCaseLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return compareOrdinal(left, right, true) < 0;
    }
};

std::string asciiLower(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return value;
}

// The input has already passed strict UTF-8 validation. If the byte limit
// lands inside a code point, omit the entire code point.
std::string truncateUtf8(std::string_view text, size_t limit) {
    if (text.size() <= limit) return std::string(text);
    auto size = limit;
    while (size != 0 && (static_cast<unsigned char>(text[size]) & 0xc0) == 0x80) --size;
    return std::string(text.substr(0, size));
}

struct Port {
    bool raw;
    uint16_t targetPort;
    std::string queue;
};

struct Printer {
    std::wstring ordinalName;
    std::string displayName;
    std::vector<Port> ports;
};

std::wstring truncateCaption(const std::wstring& text, size_t byteLimit, size_t codeUnitLimit) {
    size_t units = 0;
    size_t bytes = 0;
    while (units < text.size()) {
        const auto first = static_cast<uint16_t>(text[units]);
        const bool pair = first >= 0xd800 && first <= 0xdbff;
        const size_t nextUnits = pair ? 2 : 1;
        const size_t nextBytes = pair ? 4 : first <= 0x7f ? 1 : first <= 0x7ff ? 2 : 3;
        if (units + nextUnits > codeUnitLimit || bytes + nextBytes > byteLimit) break;
        units += nextUnits;
        bytes += nextBytes;
    }
    return text.substr(0, units);
}

std::vector<std::string> printerCaptions(const std::map<std::wstring, Printer, OrdinalIgnoreCaseLess>& printers) {
    std::vector<const Printer*> ordered;
    for (const auto& [key, printer] : printers) ordered.push_back(&printer);
    std::vector<bool> indexed(ordered.size(), false);
    std::vector<std::wstring> captions(ordered.size());

    for (;;) {
        std::map<std::wstring, std::vector<size_t>, OrdinalIgnoreCaseLess> collisions;
        for (size_t index = 0; index < ordered.size(); ++index) {
            const auto suffix = indexed[index] ? L" [" + std::to_wstring(index + 1) + L"]" : std::wstring{};
            // The suffix is ASCII, so its UTF-8 byte count equals its UTF-16
            // code-unit count. Reserve both capacities before truncating.
            captions[index] = truncateCaption(ordered[index]->ordinalName, kNameLimitBytes - suffix.size(),
                kWindowsCaptionLimitCodeUnits - suffix.size()) + suffix;
            collisions[captions[index]].push_back(index);
        }
        bool changed = false;
        bool duplicate = false;
        for (const auto& [caption, indices] : collisions) {
            if (indices.size() < 2) continue;
            duplicate = true;
            for (const auto index : indices) {
                if (!indexed[index]) {
                    indexed[index] = true;
                    changed = true;
                }
            }
        }
        if (!duplicate) break;
        // Existing short names may match a newly generated indexed caption.
        // Mark those too and repeat. Different numeric suffixes cannot collide.
        if (!changed) throw std::runtime_error("Could not generate unique PPM printer captions");
    }

    std::vector<std::string> result;
    for (const auto& caption : captions) result.push_back(encodeUtf8(caption));
    return result;
}

snmp::Oid objectOid(uint32_t group, uint32_t column) {
    auto oid = kPpmObjects;
    oid.push_back(group);
    if (group != 1) {
        oid.push_back(1); // table
        oid.push_back(1); // entry
    }
    oid.push_back(column);
    return oid;
}

void put(snmp::Mib& mib, const snmp::Oid& object, std::initializer_list<uint32_t> indices, snmp::Value value) {
    mib.objects.insert(object);
    auto instance = object;
    instance.insert(instance.end(), indices.begin(), indices.end());
    mib.values.emplace(std::move(instance), std::move(value));
}

std::string portName(std::string_view printerCaption, const Port& port) {
    const auto suffix = port.raw ? " - RAW TCP " + std::to_string(port.targetPort)
        : " - LPR " + port.queue + " (TCP 515)";
    // Retain the distinct endpoint even when the backend printer name is long.
    return truncateUtf8(printerCaption, kNameLimitBytes - suffix.size()) + suffix;
}

} // namespace

snmp::Mib PpmMib::Build(const ServiceConfiguration& configuration) {
    return BuildDirectory(configuration).baseMib;
}

PpmDirectory PpmMib::BuildDirectory(const ServiceConfiguration& configuration) {
    auto ppmConfiguration = configuration;
    ppmConfiguration.snmpPpm = true;
    ppmConfiguration.Validate();

    std::map<std::wstring, Printer, OrdinalIgnoreCaseLess> printers;
    const auto addPort = [&](const std::string& name, Port port) {
        auto ordinalName = decodePrinterName(name);
        auto [position, inserted] = printers.try_emplace(ordinalName, Printer{ ordinalName, encodeUtf8(ordinalName), {} });
        auto& printer = position->second;
        // A group may appear under differently cased spellings. Select its
        // display spelling deterministically, independently of argument order.
        if (!inserted && compareOrdinal(ordinalName, printer.ordinalName, false) < 0) {
            printer.ordinalName = std::move(ordinalName);
            printer.displayName = encodeUtf8(printer.ordinalName);
        }
        printer.ports.push_back(std::move(port));
    };
    for (const auto& raw : configuration.rawServers) {
        addPort(raw.printerName, { true, raw.port, {} });
    }
    for (const auto& lpd : configuration.lpdQueues) {
        addPort(lpd.printerName, { false, configuration.lpdPort, lpd.queueName });
    }

    const auto portCount = configuration.rawServers.size() + configuration.lpdQueues.size();
    if (printers.size() > INT_MAX || portCount > (std::numeric_limits<uint32_t>::max)()) {
        throw std::invalid_argument("Too many printers or ports for PPM indices");
    }
    const auto captions = printerCaptions(printers);

    PpmDirectory directory;
    auto& mib = directory.baseMib;
    // MIB-II description identifies the software without pretending to be a
    // physical printer or claiming a vendor's enterprise identifier.
    put(mib, { 1, 3, 6, 1, 2, 1, 1, 1 }, { 0 }, snmp::Value::Octets("PrintHub PPM print server"));
    put(mib, objectOid(1, 1), { 0 }, snmp::Value::Octets(""));
    put(mib, objectOid(1, 2), { 0 }, snmp::Value::Gauge(static_cast<uint32_t>(printers.size())));
    put(mib, objectOid(1, 3), { 0 }, snmp::Value::Gauge(static_cast<uint32_t>(portCount)));

    // Register accessible columns even if a future caller builds an empty
    // table, so SNMP can distinguish missing instances from unknown objects.
    for (uint32_t column = 2; column <= 8; ++column) mib.objects.insert(objectOid(kPrinterGroup, column));
    for (uint32_t column = 2; column <= 9; ++column) mib.objects.insert(objectOid(kPortGroup, column));
    for (const auto& object : { kHrDeviceIndex, kHrDeviceType, kHrDeviceStatus, kHrPrinterStatus, kHrPrinterErrors }) {
        mib.objects.insert(object);
    }

    uint32_t printerIndex = 0;
    for (auto& [key, printer] : printers) {
        ++printerIndex;
        directory.printers.push_back({ printerIndex, printer.ordinalName, captions[printerIndex - 1] });
        if (printer.ports.size() > INT_MAX) throw std::invalid_argument("Too many ports for a PPM printer");
        std::sort(printer.ports.begin(), printer.ports.end(), [](const Port& left, const Port& right) {
            if (left.raw != right.raw) return left.raw;
            if (left.raw) return left.targetPort < right.targetPort;
            return asciiLower(left.queue) < asciiLower(right.queue);
        });
        const auto printerValue = [&](uint32_t column, snmp::Value value) {
            put(mib, objectOid(kPrinterGroup, column), { printerIndex }, std::move(value));
        };
        printerValue(2, snmp::Value::Octets(captions[printerIndex - 1]));
        printerValue(3, snmp::Value::Octets(""));
        printerValue(4, snmp::Value::Gauge(static_cast<uint32_t>(printer.ports.size())));
        printerValue(5, snmp::Value::Integer(1));
        printerValue(6, snmp::Value::Integer(static_cast<int32_t>(printerIndex)));
        printerValue(7, snmp::Value::Octets("public"));
        printerValue(8, snmp::Value::Integer(2)); // Discover ports without making print delivery depend on status.

        put(mib, kHrDeviceIndex, { printerIndex }, snmp::Value::Integer(static_cast<int32_t>(printerIndex)));
        put(mib, kHrDeviceType, { printerIndex }, snmp::Value::ObjectId(kHrDevicePrinter));
        put(mib, kHrDeviceStatus, { printerIndex }, snmp::Value::Integer(1));
        put(mib, kHrPrinterStatus, { printerIndex }, snmp::Value::Integer(2));
        put(mib, kHrPrinterErrors, { printerIndex }, snmp::Value::Octets(std::string(2, '\0')));

        uint32_t portIndex = 0;
        for (const auto& port : printer.ports) {
            ++portIndex;
            const auto portValue = [&](uint32_t column, snmp::Value value) {
                put(mib, objectOid(kPortGroup, column), { printerIndex, portIndex }, std::move(value));
            };
            portValue(2, snmp::Value::Integer(1));
            portValue(3, snmp::Value::Octets(portName(captions[printerIndex - 1], port)));
            portValue(4, snmp::Value::Octets(port.queue));
            portValue(5, snmp::Value::Integer(port.raw ? 11 : 8));
            portValue(6, snmp::Value::Integer(port.targetPort));
            portValue(7, snmp::Value::Integer(1)); // PrintHub accepts any source port
            portValue(8, snmp::Value::Integer(0)); // no Printer MIB channel index
            portValue(9, snmp::Value::Integer(port.raw ? 2 : 1));
        }
    }
    return directory;
}

snmp::Mib PpmMib::WithMetadata(const PpmDirectory& directory,
    std::span<const ppm::StatusSample> statuses,
    std::span<const ppm::IdentitySample> identities, ppm::Clock::time_point now) {
    if (statuses.size() != directory.printers.size() || identities.size() != directory.printers.size()) {
        throw std::invalid_argument("Printer metadata count does not match the PPM directory");
    }
    auto mib = directory.baseMib;
    for (size_t position = 0; position < directory.printers.size(); ++position) {
        const auto index = directory.printers[position].index;
        const auto status = ppm::MapStatus(statuses[position], now);
        const auto update = [&](const snmp::Oid& object, snmp::Value value) {
            auto instance = object;
            instance.push_back(index);
            mib.values.insert_or_assign(std::move(instance), std::move(value));
        };
        update(kHrDeviceStatus, snmp::Value::Integer(status.deviceStatus));
        update(kHrPrinterStatus, snmp::Value::Integer(status.printerStatus));
        update(kHrPrinterErrors, snmp::Value::Octets(status.detectedErrors));
        const auto& identity = identities[position];
        const auto deviceId = identity.outcome == ppm::QueryOutcome::Success &&
            ppm::ValidateDeviceId(identity.deviceId) ? std::string_view(identity.deviceId) : std::string_view{};
        update(objectOid(kPrinterGroup, 3), snmp::Value::Octets(deviceId));
    }
    return mib;
}
