#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace snmp::ber {

struct Tlv {
    std::uint8_t tag{};
    std::string_view value;
    std::string_view raw;
};

// Each reader views its parent's contents; BER nesting is not traversed
// recursively and no wire length allocates memory.
class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {}

    // Raw capture is opt-in for callers that must preserve the original TLV.
    bool Read(Tlv& tlv) {
        if (input_.size() - offset_ < 2) {
            return false;
        }
        const auto tag = Byte(input_[offset_++]);
        // Only the single-octet tags used by SNMP are supported.
        if ((tag & 0x1FU) == 0x1FU) {
            return false;
        }
        const auto first = Byte(input_[offset_++]);
        std::size_t length = first;
        if ((first & 0x80U) != 0) {
            const auto count = static_cast<std::size_t>(first & 0x7FU);
            if (count == 0 || count > sizeof(std::size_t)
                || count > input_.size() - offset_) {
                return false;
            }
            length = 0;
            for (std::size_t index = 0; index < count; ++index) {
                if (length > (std::numeric_limits<std::size_t>::max)() / 256U) {
                    return false;
                }
                length = length * 256U + Byte(input_[offset_++]);
            }
        }
        if (length > input_.size() - offset_) {
            return false;
        }
        tlv = {tag, input_.substr(offset_, length), {}};
        offset_ += length;
        return true;
    }

    bool ReadRaw(Tlv& tlv) {
        const auto start = offset_;
        if (!Read(tlv)) {
            return false;
        }
        tlv.raw = input_.substr(start, offset_ - start);
        return true;
    }

    bool Done() const { return offset_ == input_.size(); }

private:
    static std::uint8_t Byte(char value) { return static_cast<std::uint8_t>(value); }
    std::string_view input_;
    std::size_t offset_{};
};

inline std::size_t EncodedSize(std::size_t payloadSize) {
    if (payloadSize < 128) {
        return payloadSize + 2;
    }
    std::size_t lengthBytes = 0;
    for (auto length = payloadSize; length != 0; length >>= 8U) {
        ++lengthBytes;
    }
    return payloadSize + 2 + lengthBytes;
}

inline void AppendLength(std::string& output, std::size_t length) {
    if (length < 0x80U) {
        output.push_back(static_cast<char>(length));
        return;
    }

    std::uint8_t bytes[sizeof(std::size_t)]{};
    std::size_t count = 0;
    while (length != 0) {
        bytes[count++] = static_cast<std::uint8_t>(length & 0xFFU);
        length >>= 8U;
    }
    output.push_back(static_cast<char>(0x80U | count));
    while (count != 0) {
        output.push_back(static_cast<char>(bytes[--count]));
    }
}

inline std::string EncodeTlv(std::uint8_t tag, std::string_view value) {
    std::string result;
    result.reserve(EncodedSize(value.size()));
    result.push_back(static_cast<char>(tag));
    AppendLength(result, value.size());
    result.append(value);
    return result;
}

} // namespace snmp::ber
