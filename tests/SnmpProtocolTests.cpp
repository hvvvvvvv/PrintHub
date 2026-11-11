#include <SnmpProtocol.h>

#include "SnmpTestSupport.h"

#include <limits>
#include <vector>

namespace {

const snmp::Oid kDescr{1, 3, 6, 1, 2, 1, 1, 1, 0};
const snmp::Oid kName{1, 3, 6, 1, 2, 1, 1, 5, 0};

snmp::Mib sampleMib() {
    snmp::Mib mib;
    mib.objects = {{1, 3, 6, 1, 2, 1, 1, 1}, {1, 3, 6, 1, 2, 1, 1, 5}, {1, 3, 6, 1, 2, 1, 1, 7}};
    mib.values.emplace(kDescr, snmp::Value::Integer(42));
    mib.values.emplace(kName, snmp::Value::Octets("Desk"));
    return mib;
}

void testValueEncoding() {
    struct IntegerExample { std::int32_t value; std::string payload; };
    const std::vector<IntegerExample> signedValues{
        {0, hex("00")}, {127, hex("7f")}, {128, hex("00 80")}, {255, hex("00 ff")}, {256, hex("01 00")},
        {-1, hex("ff")}, {-128, hex("80")}, {-129, hex("ff 7f")},
        {(std::numeric_limits<std::int32_t>::max)(), hex("7f ff ff ff")},
        {(std::numeric_limits<std::int32_t>::min)(), hex("80 00 00 00")}
    };
    for (const auto& example : signedValues) {
        const auto actual = snmp::Value::Integer(example.value);
        require(actual.tag == 0x02 && actual.payload == example.payload, "Minimal signed INTEGER encoding");
    }
    const std::vector<std::pair<std::uint32_t, std::string>> unsignedValues{
        {0, hex("00")}, {127, hex("7f")}, {128, hex("00 80")}, {255, hex("00 ff")},
        {256, hex("01 00")}, {(std::numeric_limits<std::uint32_t>::max)(), hex("00 ff ff ff ff")}
    };
    for (const auto& [value, expected] : unsignedValues) {
        const auto actual = snmp::Value::Gauge(value);
        require(actual.tag == 0x42 && actual.payload == expected, "Unsigned Gauge sign octet/encoding");
    }
    const auto binary = hex("00 80 ff 41 00");
    const auto octets = snmp::Value::Octets(binary);
    require(octets.tag == 0x04 && octets.payload == binary, "OCTET STRING binary payload");
    require(snmp::Value::Octets("").payload.empty(), "Empty OCTET STRING");
    const std::vector<std::pair<snmp::Oid, std::string>> oids{
        {{0, 0}, hex("00")}, {{1, 39}, hex("4f")}, {{2, 40, 127, 128, 16384}, hex("78 7f 81 00 81 80 00")},
        {kDescr, kDescrWire},
        {{2, (std::numeric_limits<std::uint32_t>::max)(), (std::numeric_limits<std::uint32_t>::max)()},
            hex("90 80 80 80 4f 8f ff ff ff 7f")}
    };
    for (const auto& [oid, expected] : oids) {
        const auto actual = snmp::Value::ObjectId(oid);
        require(actual.tag == 0x06 && actual.payload == expected, "OID base-128 encoding");
    }
    for (const auto& oid : std::vector<snmp::Oid>{{}, {1}, {3, 0}, {0, 40}, {1, 40}, snmp::Oid(129, 1)}) {
        requireThrows([&] { snmp::Value::ObjectId(oid); }, "Invalid OID accepted");
    }
    const auto largest = snmp::Value::ObjectId(snmp::Oid(128, 1));
    require(largest.payload.size() == 127, "Maximum supported OID arc count");
}

void testGetAndFixedWireResponse() {
    const auto mib = sampleMib();
    const auto request = packet(1, 0xA0, binding(kDescrWire));
    require(request == hex("30 26 02 01 01 04 06 70 75 62 6c 69 63 a0 19"
        "02 01 12 02 01 00 02 01 00 30 0e 30 0c 06 08 2b 06 01 02 01 01 01 00 05 00"),
        "Independent request fixture");
    requireResponse(snmp::Respond(request, mib), hex(
        "30 27 02 01 01 04 06 70 75 62 6c 69 63 a2 1a"
        "02 01 12 02 01 00 02 01 00 30 0f 30 0d 06 08 2b 06 01 02 01 01 01 00 02 01 2a"),
        "Fixed GetResponse bytes");
    for (const std::uint8_t version : {0, 1}) {
        const auto requested = binding(kNameWire) + binding(kDescrWire) + binding(kDescrWire);
        const auto expected = binding(kNameWire, 0x04, "Desk") + binding(kDescrWire, 0x02, hex("2a")) +
            binding(kDescrWire, 0x02, hex("2a"));
        const auto negativeId = hex("ff 7f");
        requireResponse(snmp::Respond(packet(version, 0xA0, requested, negativeId), mib),
            packet(version, 0xA2, expected, negativeId), "Version, signed request ID, binding order and duplicates");
        requireResponse(snmp::Respond(packet(version, 0xA0, ""), mib),
            packet(version, 0xA2, ""), "Respond accepts an empty binding list");
    }
}

void testMissingObjectsAndGetNext() {
    const auto mib = sampleMib();
    const auto absentInstance = hex("2b 06 01 02 01 01 01 01");
    const auto absentObject = hex("2b 06 01 02 01 01 09 00");
    const auto emptyObjectInstance = hex("2b 06 01 02 01 01 07 01");
    const auto requested = binding(kDescrWire) + binding(absentInstance) + binding(absentObject) + binding(emptyObjectInstance);
    requireResponse(snmp::Respond(packet(1, 0xA0, requested), mib),
        packet(1, 0xA2, binding(kDescrWire, 2, hex("2a")) + binding(absentInstance, 0x81) +
            binding(absentObject, 0x80) + binding(emptyObjectInstance, 0x81)),
        "v2c noSuchInstance versus noSuchObject");
    requireResponse(snmp::Respond(packet(0, 0xA0, requested), mib),
        packet(0, 0xA2, requested, hex("12"), 2, 2), "v1 noSuchName reports first missing binding and echoes all values");

    const auto before = hex("2b 06 01 02 01 01 01");
    const auto nextRequests = binding(before) + binding(kDescrWire) + binding(kNameWire);
    requireResponse(snmp::Respond(packet(1, 0xA1, nextRequests), mib),
        packet(1, 0xA2, binding(kDescrWire, 2, hex("2a")) + binding(kNameWire, 4, "Desk") + binding(kNameWire, 0x82)),
        "GetNext uses strict lexicographic successor and endOfMibView");
    requireResponse(snmp::Respond(packet(0, 0xA1, nextRequests), mib),
        packet(0, 0xA2, nextRequests, hex("12"), 2, 3), "v1 GetNext end reports noSuchName at index 3");
    requireResponse(snmp::Respond(packet(0, 0xA1, binding(kDescrWire)), mib),
        packet(0, 0xA2, binding(kNameWire, 4, "Desk")), "v1 GetNext success");

    requireResponse(snmp::Respond(packet(1, 0xA1, binding(kDescrWire)), {}),
        packet(1, 0xA2, binding(kDescrWire, 0x82)), "Empty MIB GetNext");
}

void testGetBulkAndTooBig() {
    auto mib = sampleMib();
    const auto before = hex("2b 06 01 02 01 01 01");
    const auto requestBindings = binding(before) + binding(kDescrWire);
    requireResponse(snmp::Respond(packet(1, 0xA5, requestBindings, hex("12"), 1, 3), mib),
        packet(1, 0xA2, binding(kDescrWire, 2, hex("2a")) + binding(kNameWire, 4, "Desk") + binding(kNameWire, 0x82)),
        "Bulk non-repeater, repeater and early all-end termination");
    requireResponse(snmp::Respond(packet(1, 0xA5, requestBindings, hex("12"), 0, 0), mib),
        packet(1, 0xA2, ""), "Zero bulk repetitions");
    requireResponse(snmp::Respond(packet(1, 0xA5, requestBindings, hex("12"), 9, 1), mib),
        packet(1, 0xA2, binding(kDescrWire, 2, hex("2a")) + binding(kNameWire, 4, "Desk")),
        "Bulk non-repeaters clamped to binding count");
    require(!snmp::Respond(packet(0, 0xA5, requestBindings), mib), "v1 GetBulk must be dropped");

    mib.values.insert_or_assign(kDescr, snmp::Value::Octets(std::string(snmp::kMaxMessageBytes, 'x')));
    const auto requested = binding(kDescrWire);
    requireResponse(snmp::Respond(packet(0, 0xA0, requested), mib),
        packet(0, 0xA2, requested, hex("12"), 1, 0), "v1 tooBig echoes original bindings");
    requireResponse(snmp::Respond(packet(1, 0xA0, requested), mib),
        packet(1, 0xA2, "", hex("12"), 1, 0), "v2c tooBig has empty bindings");
    const auto withMissing = requested + binding(hex("2b 06 01 02 01 01 09 00"));
    requireResponse(snmp::Respond(packet(0, 0xA0, withMissing), mib),
        packet(0, 0xA2, withMissing, hex("12"), 2, 2), "v1 name validation precedes tooBig");
    // Bulk may return the prefix that fits rather than an oversized response.
    requireResponse(snmp::Respond(packet(1, 0xA5, binding(before), hex("12"), 0, 1), mib),
        packet(1, 0xA2, ""), "Bulk value too large returns a bounded prefix");
}

void testMalformedAndSizeLimits() {
    const auto mib = sampleMib();
    const auto valid = packet(1, 0xA0, binding(kDescrWire));
    for (size_t size = 0; size < valid.size(); ++size) {
        require(!snmp::Respond(std::string_view(valid).substr(0, size), mib), "Truncated request must be dropped");
    }
    const std::vector<std::string> malformed{
        valid + '\0', hex("30 80 00 00"), packet(2, 0xA0, binding(kDescrWire)),
        packet(1, 0xA0, binding(kDescrWire), hex("12"), 0, 0, "private"),
        packet(1, 0xA0, binding(kDescrWire), ""), packet(1, 0xA0, binding(kDescrWire), hex("00 12")),
        packet(1, 0xA0, binding(kDescrWire), hex("01 00 00 00 00")),
        packet(1, 0xA0, binding(kDescrWire), hex("12"), 0xFF),
        packet(1, 0xA5, binding(kDescrWire), hex("12"), 0, 0xFF),
        packet(1, 0xA0, wireTlv(0x04, "bad binding")),
        packet(1, 0xA0, wireTlv(0x30, wireTlv(0x06, kDescrWire))),
        packet(1, 0xA0, wireTlv(0x30, wireTlv(0x04, kDescrWire) + wireTlv(5, ""))),
        packet(1, 0xA0, wireTlv(0x30, wireTlv(6, kDescrWire) + wireTlv(5, "") + wireTlv(5, ""))),
        packet(1, 0xA0, binding(kDescrWire, 5, "not NULL")),
        packet(1, 0xA0, binding(kDescrWire, 2, "")),
        packet(1, 0xA0, binding(kDescrWire, 0x40, "abc")),
        packet(1, 0xA0, binding(kDescrWire, 0x42, hex("80"))),
        packet(1, 0xA0, binding(kDescrWire, 0x42, hex("00 01"))),
        packet(1, 0xA0, binding(kDescrWire, 0x42, hex("01 00 00 00 00"))),
        packet(1, 0xA0, binding(kDescrWire, 0x30, "")),
        packet(1, 0xA0, binding(kDescrWire, 0x80, "x")),
        packet(0, 0xA0, binding(kDescrWire, 0x46, hex("00"))),
        packet(0, 0xA0, binding(kDescrWire, 0x80)),
        packet(1, 0xA0, binding("")),
        packet(1, 0xA0, binding(hex("2b 80 01"))),
        packet(1, 0xA0, binding(hex("2b 81"))),
        packet(1, 0xA0, binding(hex("2b 90 80 80 80 00"))),
        packet(1, 0xA0, binding(std::string(128, '\x2b')))
    };
    for (size_t index = 0; index < malformed.size(); ++index) {
        require(!snmp::Respond(malformed[index], mib), "Malformed request case " + std::to_string(index));
    }
    for (const std::uint8_t pdu : {0xA2, 0xA3, 0xA4, 0xA6, 0xA7}) {
        require(!snmp::Respond(packet(1, pdu, binding(kDescrWire)), mib), "Unsupported PDU");
    }
    require(!snmp::Respond(std::string(snmp::kMaxMessageBytes + 1, '\0'), mib), "Oversized garbage");
    // A valid request exactly at the cap, then one byte over (with valid BER).
    const auto boundary = packet(1, 0xA0, binding(kDescrWire, 4, std::string(4046, 'x')));
    require(boundary.size() == snmp::kMaxMessageBytes, "Exact size-limit fixture");
    requireResponse(snmp::Respond(boundary, mib), packet(1, 0xA2, binding(kDescrWire, 2, hex("2a"))),
        "Exact 4096-byte request accepted");
    const auto oversized = packet(1, 0xA0, binding(kDescrWire, 4, std::string(4047, 'x')));
    require(oversized.size() == snmp::kMaxMessageBytes + 1 && !snmp::Respond(oversized, mib),
        "Valid BER above size limit must be dropped");
}

void testValidIgnoredRequestValues() {
    const auto mib = sampleMib();
    const std::vector<std::pair<std::uint8_t, std::string>> values{
        {2, hex("ff")}, {4, std::string("a\0b", 3)}, {6, kNameWire}, {5, ""},
        {0x40, hex("7f 00 00 01")}, {0x41, hex("00 ff ff ff ff")}, {0x42, hex("00")},
        {0x43, hex("7f")}, {0x44, hex("ff 00")}, {0x46, hex("00 ff ff ff ff ff ff ff ff")},
        {0x80, ""}, {0x81, ""}, {0x82, ""}
    };
    for (const auto& [tag, payload] : values) {
        requireResponse(snmp::Respond(packet(1, 0xA0, binding(kDescrWire, tag, payload)), mib),
            packet(1, 0xA2, binding(kDescrWire, 2, hex("2a"))), "Valid request value syntax is ignored");
    }
}

} // namespace

int main() {
    try {
        runTest("Value encoding", testValueEncoding);
        runTest("Get and fixed wire response", testGetAndFixedWireResponse);
        runTest("missing objects and GetNext", testMissingObjectsAndGetNext);
        runTest("GetBulk and tooBig", testGetBulkAndTooBig);
        runTest("malformed requests and size limits", testMalformedAndSizeLimits);
        runTest("valid ignored request values", testValidIgnoredRequestValues);
    } catch (const std::exception& error) {
        std::cerr << "SNMP protocol tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
