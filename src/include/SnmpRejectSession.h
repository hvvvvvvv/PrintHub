#pragma once

#include <SnmpProtocol.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

class SnmpRejectSession {
public:
    static constexpr size_t MAX_MESSAGE_SIZE_BYTES = snmp::kMaxMessageBytes;

    // Returns no value for requests that must be silently discarded.
    static std::optional<std::string> Handle(std::string_view request);
};
