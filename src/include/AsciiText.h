#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Small ASCII-only text helpers shared by the service configuration and the
// PPM MIB code. They intentionally operate byte-wise: service arguments use
// the process ANSI code page, and the PPM wire format only requires ASCII
// case folding for queue identifiers.
namespace ascii {

inline char Lower(char character) noexcept {
    return character >= 'A' && character <= 'Z'
        ? static_cast<char>(character + ('a' - 'A'))
        : character;
}

inline std::string LowerCopy(std::string value) {
    for (auto& character : value) character = Lower(character);
    return value;
}

inline bool EqualsIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (Lower(left[index]) != Lower(right[index])) return false;
    }
    return true;
}

} // namespace ascii
