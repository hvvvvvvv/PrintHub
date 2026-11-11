#pragma once

#include "TestSupport.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace {

// Deliberately independent of SnmpBer and Value: protocol expectations must not
// silently inherit an encoder defect from the production implementation.
inline std::string wireTlv(std::uint8_t tag, std::string_view payload) {
    require(payload.size() <= 65535, "Test fixture exceeds the supported fixture size");
    std::string header(1, static_cast<char>(tag));
    if (payload.size() < 128) {
        header.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 255) {
        header += bytes({0x81, static_cast<std::uint8_t>(payload.size())});
    } else {
        header += bytes({0x82, static_cast<std::uint8_t>(payload.size() / 256),
            static_cast<std::uint8_t>(payload.size() % 256)});
    }
    return header + std::string(payload);
}

inline std::string hex(std::string_view text) {
    std::string result;
    int high = -1;
    for (const auto character : text) {
        if (character == ' ' || character == '\n' || character == '\r' || character == '\t') continue;
        const int digit = character >= '0' && character <= '9' ? character - '0'
            : character >= 'a' && character <= 'f' ? character - 'a' + 10
            : character >= 'A' && character <= 'F' ? character - 'A' + 10 : -1;
        require(digit >= 0, "Invalid hexadecimal test fixture");
        if (high < 0) high = digit;
        else {
            result.push_back(static_cast<char>(high * 16 + digit));
            high = -1;
        }
    }
    require(high < 0, "Odd-length hexadecimal test fixture");
    return result;
}

inline std::string binding(std::string_view oidPayload, std::uint8_t valueTag = 0x05,
    std::string_view value = {}) {
    return wireTlv(0x30, wireTlv(0x06, oidPayload) + wireTlv(valueTag, value));
}

inline std::string packet(std::uint8_t version, std::uint8_t pdu, std::string_view bindings,
    std::string_view id = std::string_view("\x12", 1), std::uint8_t error = 0, std::uint8_t index = 0,
    std::string_view community = "public") {
    return wireTlv(0x30, wireTlv(0x02, bytes({version})) + wireTlv(0x04, community) +
        wireTlv(pdu, wireTlv(0x02, id) + wireTlv(0x02, bytes({error})) +
            wireTlv(0x02, bytes({index})) + wireTlv(0x30, bindings)));
}

inline void requireResponse(const std::optional<std::string>& actual, const std::string& expected,
    std::string_view message) {
    require(actual.has_value(), std::string(message) + ": response was dropped");
    require(*actual == expected, std::string(message) + ": response bytes differ");
}

inline const std::string kDescrWire = hex("2b 06 01 02 01 01 01 00");
inline const std::string kNameWire = hex("2b 06 01 02 01 01 05 00");

} // namespace
