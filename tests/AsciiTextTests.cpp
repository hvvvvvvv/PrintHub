#include <AsciiText.h>

#include "TestSupport.h"

namespace {

void testLowerAndLowerCopy() {
    const std::string upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const std::string lower = "abcdefghijklmnopqrstuvwxyz";
    for (size_t index = 0; index < upper.size(); ++index) {
        require(ascii::Lower(upper[index]) == lower[index], "ASCII uppercase folding");
        require(ascii::Lower(lower[index]) == lower[index], "Lowercase must stay unchanged");
    }
    for (unsigned int value = 0; value < 256; ++value) {
        const auto character = static_cast<char>(static_cast<unsigned char>(value));
        if (upper.find(character) == std::string::npos) {
            require(ascii::Lower(character) == character, "Non-uppercase byte changed");
        }
    }
    const auto input = std::string("Az09-_\t\0Z", 9) + bytes({0x80, 0xC4, 0xFF});
    const auto expected = std::string("az09-_\t\0z", 9) + bytes({0x80, 0xC4, 0xFF});
    require(ascii::LowerCopy(input) == expected, "LowerCopy must preserve binary and non-ASCII bytes");
    require(input[0] == 'A' && input[8] == 'Z', "LowerCopy mutated its source");
    require(ascii::LowerCopy("").empty(), "Empty LowerCopy");
}

void testEquality() {
    require(ascii::EqualsIgnoreCase("", ""), "Empty strings compare equal");
    require(ascii::EqualsIgnoreCase("PrInTeR-12", "printer-12"), "Mixed-case comparison");
    require(!ascii::EqualsIgnoreCase("queue", "queues") && !ascii::EqualsIgnoreCase("", "x"), "Different lengths");
    require(!ascii::EqualsIgnoreCase("one", "two"), "Different letters");
    require(!ascii::EqualsIgnoreCase("queue ", "queue") && !ascii::EqualsIgnoreCase("a-b", "a_b"),
        "No trimming or punctuation normalization");
    const auto left = std::string("A\0B", 3) + bytes({0xFF});
    const auto right = std::string("a\0b", 3) + bytes({0xFF});
    require(ascii::EqualsIgnoreCase(left, right), "Embedded NUL must not terminate comparison");
    // Same length, differing only after the NUL: a C-string style comparison
    // would wrongly report equality here.
    require(!ascii::EqualsIgnoreCase(std::string("A\0B", 3), std::string("a\0C", 3)),
        "Bytes after an embedded NUL must still be compared");
    require(!ascii::EqualsIgnoreCase(bytes({0xC4}), bytes({0xE4})), "No non-ASCII case folding");
    const std::string backing = "!aBc?";
    require(ascii::EqualsIgnoreCase(std::string_view(backing).substr(1, 3), "ABC"), "Bounded string_view input");
}

} // namespace

int main() {
    try {
        runTest("Lower and LowerCopy", testLowerAndLowerCopy);
        runTest("EqualsIgnoreCase", testEquality);
    } catch (const std::exception& error) {
        std::cerr << "ASCII text tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
