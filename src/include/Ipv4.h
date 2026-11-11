#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

// Returns the IPv4 address in host byte order, with decimal octets.
inline std::optional<uint32_t> ParseIpv4Address(std::string_view value) {
    uint32_t address = 0;
    std::size_t start = 0;
    for (int part = 0; part < 4; ++part) {
        const auto end = value.find('.', start);
        if ((part == 3 && end != std::string_view::npos) ||
            (part != 3 && end == std::string_view::npos)) {
            return std::nullopt;
        }
        const auto length = (end == std::string_view::npos ? value.size() : end) - start;
        if (length == 0 || length > 3) return std::nullopt;
        unsigned int number = 0;
        const auto* first = value.data() + start;
        const auto* last = first + length;
        const auto [position, error] = std::from_chars(first, last, number);
        if (error != std::errc{} || position != last || number > 255) return std::nullopt;
        // Decimal octets preserve the accepted leading-zero spelling.
        address = (address << 8) | static_cast<uint32_t>(number);
        if (part != 3) start = end + 1;
    }
    return address;
}

inline bool IsIpv4Address(std::string_view value) {
    return ParseIpv4Address(value).has_value();
}
