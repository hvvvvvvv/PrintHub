#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace ppm {

using Clock = std::chrono::steady_clock;

enum class QueryOutcome : uint32_t {
    Success,
    NotFound,
    Unsupported,
    Failed,
};

struct StatusSample {
    QueryOutcome outcome = QueryOutcome::Failed;
    uint32_t flags = 0;
    Clock::time_point observedAt{};
};

struct IdentitySample {
    QueryOutcome outcome = QueryOutcome::Failed;
    std::string deviceId;
};

struct MappedStatus {
    int32_t deviceStatus = 1; // hrDeviceStatus: unknown
    int32_t printerStatus = 2; // hrPrinterStatus: unknown
    std::string detectedErrors;
};

MappedStatus MapStatus(const StatusSample& sample, Clock::time_point now);
bool ValidateDeviceId(std::string_view deviceId);

} // namespace ppm
