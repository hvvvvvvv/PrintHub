#include <SnmpProtocol.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace snmp {
namespace {

constexpr std::uint8_t kInteger = 0x02;
constexpr std::uint8_t kOctets = 0x04;
constexpr std::uint8_t kNull = 0x05;
constexpr std::uint8_t kOid = 0x06;
constexpr std::uint8_t kSequence = 0x30;
constexpr std::uint8_t kGet = 0xA0;
constexpr std::uint8_t kNext = 0xA1;
constexpr std::uint8_t kResponse = 0xA2;
constexpr std::uint8_t kBulk = 0xA5;
constexpr std::uint8_t kNoSuchObject = 0x80;
constexpr std::uint8_t kNoSuchInstance = 0x81;
constexpr std::uint8_t kEndOfMibView = 0x82;
constexpr std::size_t kMaxOidArcs = 128;

struct Tlv {
    std::uint8_t tag{};
    std::string_view value;
    std::string_view raw;
};

// SNMP has a fixed, shallow grammar. Each reader views its parent's contents;
// BER nesting is never traversed recursively and no wire length allocates memory.
class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {}

    bool Read(Tlv& tlv) {
        const auto start = offset_;
        if (input_.size() - offset_ < 2) {
            return false;
        }
        tlv.tag = Byte(input_[offset_++]);
        // Only the single-octet tags used by SNMP are supported.
        if ((tlv.tag & 0x1FU) == 0x1FU) {
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
        tlv.value = input_.substr(offset_, length);
        offset_ += length;
        tlv.raw = input_.substr(start, offset_ - start);
        return true;
    }

    bool Done() const { return offset_ == input_.size(); }

private:
    static std::uint8_t Byte(char value) { return static_cast<std::uint8_t>(value); }
    std::string_view input_;
    std::size_t offset_{};
};

bool DecodeInteger(const Tlv& tlv, std::int32_t& value) {
    if (tlv.tag != kInteger || tlv.value.empty() || tlv.value.size() > 4) {
        return false;
    }
    const auto first = static_cast<std::uint8_t>(tlv.value.front());
    if (tlv.value.size() > 1) {
        const auto second = static_cast<std::uint8_t>(tlv.value[1]);
        if ((first == 0 && (second & 0x80U) == 0)
            || (first == 0xFF && (second & 0x80U) != 0)) {
            return false; // BER INTEGER uses the shortest signed representation.
        }
    }
    std::int64_t decoded = (first & 0x80U) != 0 ? -1 : 0;
    for (const auto byte : tlv.value) {
        decoded = decoded * 256 + static_cast<std::uint8_t>(byte);
    }
    value = static_cast<std::int32_t>(decoded);
    return true;
}

bool ReadInteger(Reader& reader, std::int32_t& value) {
    Tlv tlv;
    return reader.Read(tlv) && DecodeInteger(tlv, value);
}

bool DecodeOid(std::string_view input, Oid& oid) {
    if (input.empty()) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < input.size()) {
        std::uint64_t number = 0;
        const auto start = offset;
        const auto limit = oid.empty()
            ? static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 80U
            : static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)());
        while (true) {
            if (offset == input.size()) {
                return false;
            }
            const auto byte = static_cast<std::uint8_t>(input[offset++]);
            if (offset == start + 1 && byte == 0x80) {
                return false; // A base-128 subidentifier cannot have leading zeroes.
            }
            if (number > (limit - (byte & 0x7FU)) / 128U) {
                return false;
            }
            number = number * 128U + (byte & 0x7FU);
            if ((byte & 0x80U) == 0) {
                break;
            }
        }
        if (oid.empty()) {
            const auto first = number < 40 ? 0U : number < 80 ? 1U : 2U;
            oid.push_back(first);
            oid.push_back(static_cast<std::uint32_t>(number - first * 40U));
        }
        else {
            oid.push_back(static_cast<std::uint32_t>(number));
        }
        if (oid.size() > kMaxOidArcs) {
            return false;
        }
    }
    return true;
}

bool ValidUnsigned(std::string_view contents, std::uint64_t limit) {
    if (contents.empty() || (static_cast<std::uint8_t>(contents.front()) & 0x80U) != 0) {
        return false;
    }
    if (contents.size() > 1 && contents.front() == '\0'
        && (static_cast<std::uint8_t>(contents[1]) & 0x80U) == 0) {
        return false; // The leading sign octet is only valid when required.
    }
    std::uint64_t number = 0;
    for (const auto byte : contents) {
        const auto next = static_cast<std::uint8_t>(byte);
        if (number > (limit - next) / 256U) {
            return false;
        }
        number = number * 256U + next;
    }
    return true;
}

bool ValidRequestValue(const Tlv& value, std::int32_t version) {
    // RFC 3416 ignores a request VarBind's value, but its ASN.1 syntax must
    // still be valid. Check only the permitted primitive syntaxes, without
    // traversing constructed values or allocating based on BER lengths.
    switch (value.tag) {
    case kInteger: {
        std::int32_t integer;
        return DecodeInteger(value, integer);
    }
    case kOctets:
    case 0x44: // Opaque
        return true;
    case kOid: {
        Oid oid;
        return DecodeOid(value.value, oid);
    }
    case kNull:
        return value.value.empty();
    case 0x40: // IpAddress
        return value.value.size() == 4;
    case 0x41: // Counter32
    case 0x42: // Gauge32 / Unsigned32
    case 0x43: // TimeTicks
        return value.value.size() <= 5
            && ValidUnsigned(value.value, (std::numeric_limits<std::uint32_t>::max)());
    case 0x46: // Counter64 is available only in v2c.
        return version == 1 && value.value.size() <= 9
            && ValidUnsigned(value.value, (std::numeric_limits<std::uint64_t>::max)());
    case kNoSuchObject:
    case kNoSuchInstance:
    case kEndOfMibView:
        return version == 1 && value.value.empty();
    default:
        return false;
    }
}

void AppendSubidentifier(std::string& output, std::uint64_t value) {
    std::uint8_t bytes[10]{};
    std::size_t count = 0;
    do {
        bytes[count++] = static_cast<std::uint8_t>(value & 0x7FU);
        value >>= 7U;
    } while (value != 0);
    while (count != 0) {
        --count;
        output.push_back(static_cast<char>(bytes[count] | (count != 0 ? 0x80U : 0)));
    }
}

std::string EncodeOid(const Oid& oid) {
    if (oid.size() < 2 || oid.size() > kMaxOidArcs || oid[0] > 2
        || (oid[0] < 2 && oid[1] > 39)) {
        throw std::invalid_argument("Invalid SNMP object identifier");
    }
    std::string output;
    AppendSubidentifier(output, static_cast<std::uint64_t>(oid[0]) * 40U + oid[1]);
    for (std::size_t index = 2; index < oid.size(); ++index) {
        AppendSubidentifier(output, oid[index]);
    }
    return output;
}

std::size_t EncodedSize(std::size_t payloadSize) {
    if (payloadSize < 128) {
        return payloadSize + 2;
    }
    std::size_t lengthBytes = 0;
    for (auto length = payloadSize; length != 0; length >>= 8U) {
        ++lengthBytes;
    }
    return payloadSize + 2 + lengthBytes;
}

std::string TlvBytes(std::uint8_t tag, std::string_view value) {
    std::string result;
    result.reserve(EncodedSize(value.size()));
    result.push_back(static_cast<char>(tag));
    if (value.size() < 128) {
        result.push_back(static_cast<char>(value.size()));
    }
    else {
        std::uint8_t bytes[sizeof(std::size_t)]{};
        std::size_t count = 0;
        for (auto length = value.size(); length != 0; length >>= 8U) {
            bytes[count++] = static_cast<std::uint8_t>(length & 0xFFU);
        }
        result.push_back(static_cast<char>(0x80U | count));
        while (count != 0) {
            result.push_back(static_cast<char>(bytes[--count]));
        }
    }
    result.append(value);
    return result;
}

struct Request {
    std::int32_t version{};
    std::int32_t id{};
    std::int32_t nonRepeaters{};
    std::int32_t maxRepetitions{};
    std::uint8_t pdu{};
    std::vector<Oid> oids;
    std::string_view originalBindings;
};

bool ParseRequest(std::string_view wire, Request& request) {
    if (wire.empty() || wire.size() > kMaxMessageBytes) {
        return false;
    }
    Reader outer(wire);
    Tlv message;
    if (!outer.Read(message) || message.tag != kSequence || !outer.Done()) {
        return false;
    }
    Reader body(message.value);
    Tlv community;
    Tlv pdu;
    if (!ReadInteger(body, request.version) || (request.version != 0 && request.version != 1)
        || !body.Read(community) || community.tag != kOctets || community.value != "public"
        || !body.Read(pdu) || !body.Done()
        || (pdu.tag != kGet && pdu.tag != kNext && pdu.tag != kBulk)
        || (pdu.tag == kBulk && request.version != 1)) {
        return false;
    }
    request.pdu = pdu.tag;
    Reader pduBody(pdu.value);
    Tlv bindings;
    if (!ReadInteger(pduBody, request.id)
        || !ReadInteger(pduBody, request.nonRepeaters)
        || !ReadInteger(pduBody, request.maxRepetitions)
        || request.nonRepeaters < 0 || request.maxRepetitions < 0
        || !pduBody.Read(bindings) || bindings.tag != kSequence || !pduBody.Done()) {
        return false;
    }
    request.originalBindings = bindings.value;
    Reader list(bindings.value);
    while (!list.Done()) {
        Tlv binding;
        if (!list.Read(binding) || binding.tag != kSequence) {
            return false;
        }
        Reader fields(binding.value);
        Tlv name;
        Tlv value;
        Oid oid;
        if (!fields.Read(name) || name.tag != kOid || !DecodeOid(name.value, oid)
            || !fields.Read(value) || !ValidRequestValue(value, request.version)
            || !fields.Done()) {
            return false;
        }
        request.oids.push_back(std::move(oid));
    }
    return true;
}

class ResponseEncoder {
public:
    explicit ResponseEncoder(const Request& request)
        : version_(TlvBytes(kInteger, Value::Integer(request.version).payload)),
          id_(TlvBytes(kInteger, Value::Integer(request.id).payload)) {}

    bool Fits(std::size_t bindingsSize, std::int32_t error = 0, std::int32_t index = 0) const {
        if (bindingsSize > kMaxMessageBytes) {
            return false;
        }
        const auto pduSize = id_.size() + EncodedSize(Value::Integer(error).payload.size())
            + EncodedSize(Value::Integer(index).payload.size()) + EncodedSize(bindingsSize);
        return EncodedSize(version_.size() + 8 + EncodedSize(pduSize)) <= kMaxMessageBytes;
    }

    std::optional<std::string> Encode(std::string_view bindings,
        std::int32_t error = 0, std::int32_t index = 0) const {
        if (!Fits(bindings.size(), error, index)) {
            return std::nullopt;
        }
        auto pdu = id_ + TlvBytes(kInteger, Value::Integer(error).payload)
            + TlvBytes(kInteger, Value::Integer(index).payload) + TlvBytes(kSequence, bindings);
        return TlvBytes(kSequence, version_ + TlvBytes(kOctets, "public") + TlvBytes(kResponse, pdu));
    }

private:
    std::string version_;
    std::string id_;
};

std::optional<std::string> EncodeBinding(const Oid& oid, const Value& value) {
    if (value.payload.size() > kMaxMessageBytes) {
        return std::nullopt;
    }
    const auto encodedOid = EncodeOid(oid);
    const auto size = EncodedSize(encodedOid.size()) + EncodedSize(value.payload.size());
    if (EncodedSize(size) > kMaxMessageBytes) {
        return std::nullopt;
    }
    return TlvBytes(kSequence, TlvBytes(kOid, encodedOid) + TlvBytes(value.tag, value.payload));
}

bool KnownObject(const Oid& oid, const Mib& mib) {
    // Object metadata distinguishes a missing row/scalar instance from an object
    // the agent does not implement, including empty and multi-index tables.
    for (const auto& object : mib.objects) {
        if (oid.size() >= object.size()
            && std::equal(object.begin(), object.end(), oid.begin())) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> TooBig(const Request& request, const ResponseEncoder& encoder) {
    // v1 preserves the original bindings; v2c uses the alternate empty response.
    return encoder.Encode(request.version == 0 ? request.originalBindings : std::string_view{}, 1, 0);
}

} // namespace

Value Value::Integer(std::int32_t value) {
    const auto bits = static_cast<std::uint32_t>(value);
    std::string bytes;
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<char>((bits >> shift) & 0xFFU));
    }
    std::size_t start = 0;
    while (start < 3) {
        const auto byte = static_cast<std::uint8_t>(bytes[start]);
        const auto next = static_cast<std::uint8_t>(bytes[start + 1]);
        if ((byte == 0 && (next & 0x80U) == 0) || (byte == 0xFF && (next & 0x80U) != 0)) {
            ++start;
        }
        else {
            break;
        }
    }
    return {kInteger, bytes.substr(start)};
}

Value Value::Gauge(std::uint32_t value) {
    std::uint8_t bytes[5]{};
    std::size_t count = 0;
    do {
        bytes[count++] = static_cast<std::uint8_t>(value & 0xFFU);
        value >>= 8U;
    } while (value != 0);
    std::string payload;
    if ((bytes[count - 1] & 0x80U) != 0) {
        payload.push_back('\0');
    }
    while (count != 0) {
        payload.push_back(static_cast<char>(bytes[--count]));
    }
    return {0x42, std::move(payload)};
}

Value Value::Octets(std::string_view value) { return {kOctets, std::string(value)}; }
Value Value::ObjectId(const Oid& value) { return {kOid, EncodeOid(value)}; }

std::optional<std::string> Respond(std::string_view wire, const Mib& mib) {
    Request request;
    if (!ParseRequest(wire, request)) {
        return std::nullopt;
    }
    const ResponseEncoder encoder(request);
    std::string bindings;

    if (request.pdu == kBulk) {
        const auto nonRepeaters = (std::min)(request.oids.size(),
            static_cast<std::size_t>(request.nonRepeaters));
        const auto repeatStart = request.oids.begin() + static_cast<std::ptrdiff_t>(nonRepeaters);
        std::vector<Oid> cursors(repeatStart, request.oids.end());
        const auto appendNext = [&](Oid& cursor, bool& ended) {
            const auto found = mib.values.upper_bound(cursor);
            ended = found == mib.values.end();
            const auto binding = ended ? EncodeBinding(cursor, {kEndOfMibView, {}})
                : EncodeBinding(found->first, found->second);
            if (!binding || !encoder.Fits(bindings.size() + binding->size())) {
                return false;
            }
            bindings += *binding;
            if (!ended) {
                cursor = found->first;
            }
            return true;
        };
        for (std::size_t index = 0; index < nonRepeaters; ++index) {
            auto cursor = request.oids[index];
            bool ended = false;
            if (!appendNext(cursor, ended)) {
                return encoder.Encode(bindings);
            }
        }
        for (std::int32_t repetition = 0;
             repetition < request.maxRepetitions && !cursors.empty(); ++repetition) {
            bool allEnded = true;
            for (auto& cursor : cursors) {
                bool ended = false;
                if (!appendNext(cursor, ended)) {
                    return encoder.Encode(bindings);
                }
                allEnded = allEnded && ended;
            }
            // RFC 3416 permits omitting remaining all-endOfMibView repetitions.
            if (allEnded) {
                break;
            }
        }
        return encoder.Encode(bindings);
    }

    // Name validation precedes size handling in v1, so a missing later name
    // reports noSuchName even when an earlier object's value would be too big.
    if (request.version == 0) {
        for (std::size_t index = 0; index < request.oids.size(); ++index) {
            const auto& oid = request.oids[index];
            const auto found = request.pdu == kNext ? mib.values.upper_bound(oid) : mib.values.find(oid);
            if (found == mib.values.end()) {
                return encoder.Encode(request.originalBindings, 2, static_cast<std::int32_t>(index + 1));
            }
        }
    }
    for (std::size_t index = 0; index < request.oids.size(); ++index) {
        const auto& oid = request.oids[index];
        const auto found = request.pdu == kNext ? mib.values.upper_bound(oid) : mib.values.find(oid);
        auto binding = found != mib.values.end() ? EncodeBinding(found->first, found->second)
            : EncodeBinding(oid, {request.pdu == kNext ? kEndOfMibView
                : KnownObject(oid, mib) ? kNoSuchInstance : kNoSuchObject, {}});
        if (!binding || !encoder.Fits(bindings.size() + binding->size())) {
            return TooBig(request, encoder);
        }
        bindings += *binding;
    }
    return encoder.Encode(bindings);
}

} // namespace snmp
