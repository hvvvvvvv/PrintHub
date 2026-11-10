#include <PrinterMetadata.h>

#include <Windows.h>
#include <winspool.h>

#include <chrono>

namespace ppm {
namespace {

constexpr uint32_t kDownFlags = PRINTER_STATUS_OFFLINE | PRINTER_STATUS_NOT_AVAILABLE |
    PRINTER_STATUS_PENDING_DELETION | PRINTER_STATUS_PAUSED | PRINTER_STATUS_PAPER_OUT |
    PRINTER_STATUS_PAPER_JAM | PRINTER_STATUS_DOOR_OPEN | PRINTER_STATUS_NO_TONER |
    PRINTER_STATUS_OUTPUT_BIN_FULL;
constexpr uint32_t kWarningFlags = PRINTER_STATUS_TONER_LOW | PRINTER_STATUS_MANUAL_FEED |
    PRINTER_STATUS_PAPER_PROBLEM | PRINTER_STATUS_OUT_OF_MEMORY | PRINTER_STATUS_PAGE_PUNT |
    PRINTER_STATUS_USER_INTERVENTION | PRINTER_STATUS_ERROR;
constexpr uint32_t kKnownFlags = kDownFlags | kWarningFlags | PRINTER_STATUS_WARMING_UP |
    PRINTER_STATUS_INITIALIZING | PRINTER_STATUS_PRINTING | PRINTER_STATUS_BUSY |
    PRINTER_STATUS_IO_ACTIVE | PRINTER_STATUS_PROCESSING | PRINTER_STATUS_WAITING |
    PRINTER_STATUS_POWER_SAVE | PRINTER_STATUS_SERVER_UNKNOWN;
constexpr auto kMaximumStatusAge = std::chrono::seconds(30);

bool whitespace(unsigned char character) {
    return character == ' ' || character == '\t' || character == '\v' || character == '\r' ||
        character == '\n' || character == '\f';
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && whitespace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && whitespace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

void errorBit(std::string& bytes, size_t bit) {
    bytes[bit / 8] = static_cast<char>(static_cast<unsigned char>(bytes[bit / 8]) | (0x80 >> (bit % 8)));
}

} // namespace

MappedStatus MapStatus(const StatusSample& sample, Clock::time_point now) {
    MappedStatus result;
    result.detectedErrors.assign(2, '\0');
    if (sample.outcome != QueryOutcome::Success || now - sample.observedAt > kMaximumStatusAge ||
        (sample.flags & PRINTER_STATUS_SERVER_UNKNOWN) || (sample.flags & ~kKnownFlags)) return result;

    // hrPrinterDetectedErrorState numbers bits from the most significant bit
    // of the first byte. Set only faults actually observed in the spooler.
    if (sample.flags & PRINTER_STATUS_PAPER_OUT) errorBit(result.detectedErrors, 1);
    if (sample.flags & PRINTER_STATUS_TONER_LOW) errorBit(result.detectedErrors, 2);
    if (sample.flags & PRINTER_STATUS_NO_TONER) errorBit(result.detectedErrors, 3);
    if (sample.flags & PRINTER_STATUS_DOOR_OPEN) errorBit(result.detectedErrors, 4);
    if (sample.flags & PRINTER_STATUS_PAPER_JAM) errorBit(result.detectedErrors, 5);
    if (sample.flags & PRINTER_STATUS_OFFLINE) errorBit(result.detectedErrors, 6);
    if (sample.flags & PRINTER_STATUS_OUTPUT_BIN_FULL) errorBit(result.detectedErrors, 12);
    if (sample.flags & (PRINTER_STATUS_USER_INTERVENTION | PRINTER_STATUS_ERROR |
        PRINTER_STATUS_PAPER_PROBLEM | PRINTER_STATUS_OUT_OF_MEMORY | PRINTER_STATUS_PAGE_PUNT)) {
        errorBit(result.detectedErrors, 7);
    }

    if (sample.flags & kDownFlags) {
        result.deviceStatus = 5;
        result.printerStatus = 1;
        return result;
    }
    result.deviceStatus = sample.flags & kWarningFlags ? 3 : 2;
    if (sample.flags & (PRINTER_STATUS_WARMING_UP | PRINTER_STATUS_INITIALIZING)) result.printerStatus = 5;
    else if (sample.flags & PRINTER_STATUS_PRINTING) result.printerStatus = 4;
    else if (sample.flags & (PRINTER_STATUS_BUSY | PRINTER_STATUS_IO_ACTIVE | PRINTER_STATUS_PROCESSING)) result.printerStatus = 1;
    else result.printerStatus = 3;
    return result;
}

bool ValidateDeviceId(std::string_view deviceId) {
    if (deviceId.empty() || deviceId.size() > 1023) return false;
    for (const auto character : deviceId) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte > 0x7f || (byte < 0x20 && !whitespace(byte))) return false;
    }

    bool manufacturer = false;
    bool model = false;
    auto remaining = trim(deviceId);
    while (!remaining.empty()) {
        const auto terminator = remaining.find(';');
        if (terminator == std::string_view::npos) return false;
        const auto capability = remaining.substr(0, terminator);
        const auto colon = capability.find(':');
        if (colon == std::string_view::npos || capability.find(':', colon + 1) != std::string_view::npos) return false;
        const auto key = trim(capability.substr(0, colon));
        if (key.empty() || key.find(',') != std::string_view::npos) return false;
        auto values = capability.substr(colon + 1);
        for (;;) {
            const auto comma = values.find(',');
            if (trim(values.substr(0, comma)).empty()) return false;
            if (comma == std::string_view::npos) break;
            values.remove_prefix(comma + 1);
        }
        manufacturer = manufacturer || key == "MFG" || key == "MANUFACTURER";
        model = model || key == "MDL" || key == "MODEL";
        remaining = trim(remaining.substr(terminator + 1));
    }
    return manufacturer && model;
}

} // namespace ppm
