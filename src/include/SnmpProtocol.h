#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace snmp {

using Oid = std::vector<std::uint32_t>;

// The payload contains the BER value contents, without its tag or length.
struct Value {
    std::uint8_t tag;
    std::string payload;

    static Value Integer(std::int32_t value);
    static Value Gauge(std::uint32_t value);
    static Value Octets(std::string_view value);
    static Value ObjectId(const Oid& value);
};

struct Mib {
    std::map<Oid, Value> values;
    // Scalar/column object OIDs, without their instance indexes. This includes
    // objects with no instances so missing objects and instances stay distinct.
    std::set<Oid> objects;
};
inline constexpr std::size_t kMaxMessageBytes = 4096;

// Unsupported or malformed requests, including unknown communities, are dropped.
std::optional<std::string> Respond(std::string_view request, const Mib& mib);

} // namespace snmp
