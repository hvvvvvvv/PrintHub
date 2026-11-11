#include <SnmpRejectSession.h>

#include "SnmpTestSupport.h"

#include <vector>

namespace {

void testFixedV1AndV2Responses() {
    const auto v1 = hex("30 26 02 01 00 04 06 70 75 62 6c 69 63 a2 19"
        "02 01 12 02 01 02 02 01 01 30 0e 30 0c 06 08 2b 06 01 02 01 01 01 00 05 00");
    const auto v2Get = hex("30 26 02 01 01 04 06 70 75 62 6c 69 63 a2 19"
        "02 01 12 02 01 00 02 01 00 30 0e 30 0c 06 08 2b 06 01 02 01 01 01 00 80 00");
    const auto v2Next = hex("30 26 02 01 01 04 06 70 75 62 6c 69 63 a2 19"
        "02 01 12 02 01 00 02 01 00 30 0e 30 0c 06 08 2b 06 01 02 01 01 01 00 82 00");
    for (const std::uint8_t pdu : {0xA0, 0xA1}) {
        requireResponse(SnmpRejectSession::Handle(packet(0, pdu, binding(kDescrWire))), v1,
            "Fixed v1 Get/GetNext noSuchName response");
        requireResponse(SnmpRejectSession::Handle(packet(1, pdu, binding(kDescrWire))),
            pdu == 0xA0 ? v2Get : v2Next, "Fixed v2c Get/GetNext exception response");
    }
    const auto requested = binding(kNameWire) + binding(kDescrWire) + binding(kNameWire);
    for (const std::uint8_t version : {0, 1}) {
        for (const std::uint8_t pdu : {0xA0, 0xA1}) {
            const auto tag = static_cast<std::uint8_t>(pdu == 0xA0 ? 0x80 : 0x82);
            const auto expected = version == 0
                ? packet(version, 0xA2, requested, hex("00 80"), 2, 1)
                : packet(version, 0xA2, binding(kNameWire, tag) + binding(kDescrWire, tag) + binding(kNameWire, tag),
                    hex("00 80"));
            requireResponse(SnmpRejectSession::Handle(packet(version, pdu, requested, hex("00 80"))),
                expected, "Multiple bindings preserve order, duplicates and request ID");
        }
    }
}

void testOriginalTlvPreservation() {
    const auto community = hex("04 81 06") + "public";
    const auto id = hex("02 81 02 00 80");
    const auto oid = hex("06 81 08") + kDescrWire;
    const auto requestBinding = wireTlv(0x30, oid + hex("05 00"));
    const auto list = bytes({0x30, 0x81, static_cast<std::uint8_t>(requestBinding.size())}) + requestBinding;
    for (const std::uint8_t version : {0, 1}) {
        const auto versionTlv = bytes({0x02, 0x81, 0x01, version});
        const auto request = wireTlv(0x30, versionTlv + community +
            wireTlv(0xA0, id + hex("02 01 00 02 01 00") + list));
        const auto responseFields = version == 0
            ? id + hex("02 01 02 02 01 01") + list
            : id + hex("02 01 00 02 01 00") + wireTlv(0x30, wireTlv(0x30, oid + hex("80 00")));
        requireResponse(SnmpRejectSession::Handle(request),
            wireTlv(0x30, versionTlv + community + wireTlv(0xA2, responseFields)),
            "Raw version/community/ID/OID and v1 VarBindList TLVs must be preserved");
    }
}

void testMalformedAndUnsupportedRequests() {
    const auto valid = packet(1, 0xA0, binding(kDescrWire));
    for (size_t size = 0; size < valid.size(); ++size) {
        require(!SnmpRejectSession::Handle(std::string_view(valid).substr(0, size)), "Truncated BER answered");
    }
    const std::vector<std::string> malformed{
        valid + '\0', hex("30 80 00 00"), hex("3f 00"), packet(2, 0xA0, binding(kDescrWire)),
        packet(1, 0xA0, binding(kDescrWire), hex("12"), 0, 0, "private"),
        packet(1, 0xA0, binding(kDescrWire), hex("12"), 0, 0, "Public"),
        packet(1, 0xA0, binding(kDescrWire), hex("12"), 0, 0, ""),
        packet(1, 0xA0, ""),
        packet(1, 0xA0, wireTlv(0x04, "not a sequence")),
        packet(1, 0xA0, wireTlv(0x30, wireTlv(0x06, kDescrWire))),
        packet(1, 0xA0, wireTlv(0x30, wireTlv(0x04, kDescrWire) + hex("05 00"))),
        packet(1, 0xA0, wireTlv(0x30, wireTlv(0x06, kDescrWire) + hex("05 00 05 00"))),
        packet(1, 0xA0, binding(kDescrWire, 0x05, "x")),
        packet(1, 0xA0, binding(kDescrWire, 0x02, hex("01"))),
        packet(1, 0xA0, binding(kDescrWire, 0x80)),
        packet(1, 0xA0, hex("30 7f 06 01 2b 05 00"))
    };
    for (size_t index = 0; index < malformed.size(); ++index) {
        require(!SnmpRejectSession::Handle(malformed[index]), "Invalid request case " + std::to_string(index));
    }
    for (const std::uint8_t pdu : {0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7}) {
        for (const std::uint8_t version : {0, 1}) {
            require(!SnmpRejectSession::Handle(packet(version, pdu, binding(kDescrWire))), "Unsupported PDU answered");
        }
    }
    const auto body = hex("02 01 01 04 06") + "public";
    require(!SnmpRejectSession::Handle(wireTlv(0x30, body +
        wireTlv(0xA0, hex("04 01 12 02 01 00 02 01 00") + wireTlv(0x30, binding(kDescrWire))))),
        "Non-INTEGER request ID");
    require(!SnmpRejectSession::Handle(wireTlv(0x30, body +
        wireTlv(0xA0, hex("02 01 12 04 01 00 02 01 00") + wireTlv(0x30, binding(kDescrWire))))),
        "Non-INTEGER error status");
    require(!SnmpRejectSession::Handle(wireTlv(0x30, body +
        wireTlv(0xA0, hex("02 01 12 02 01 00 04 01 00") + wireTlv(0x30, binding(kDescrWire))))),
        "Non-INTEGER error index");
    require(!SnmpRejectSession::Handle(wireTlv(0x30, body +
        wireTlv(0xA0, hex("02 01 12 02 01 00 02 01 00") + wireTlv(0x30, binding(kDescrWire)) + hex("05 00")))),
        "Trailing PDU field");
}

void testMessageSizeBoundary() {
    std::string bindings;
    for (int index = 0; index < 289; ++index) bindings += binding(kDescrWire);
    // 289 standard bindings plus one longer, syntactically valid OID gives 4096 bytes.
    const auto longOid = kDescrWire + hex("01 01 01 01");
    bindings += binding(longOid);
    const auto atLimit = packet(1, 0xA0, bindings);
    require(atLimit.size() == SnmpRejectSession::MAX_MESSAGE_SIZE_BYTES, "4096-byte rejection fixture");
    std::string expectedBindings;
    for (int index = 0; index < 289; ++index) expectedBindings += binding(kDescrWire, 0x80);
    expectedBindings += binding(longOid, 0x80);
    requireResponse(SnmpRejectSession::Handle(atLimit), packet(1, 0xA2, expectedBindings),
        "Exact maximum-size request/response");
    const auto tooLarge = packet(1, 0xA0, bindings + binding(kDescrWire));
    require(tooLarge.size() > SnmpRejectSession::MAX_MESSAGE_SIZE_BYTES && !SnmpRejectSession::Handle(tooLarge),
        "Oversized structurally valid request");
    require(!SnmpRejectSession::Handle(std::string(SnmpRejectSession::MAX_MESSAGE_SIZE_BYTES + 1, '\0')),
        "Oversized garbage request");
}

void testInvalidPrimitiveContents() {
    const std::vector<std::string> invalidOids{
        "", hex("81"), hex("2b 81"), hex("80 01"), hex("2b 80 01")
    };
    const std::vector<std::string> invalidIntegers{
        "", hex("00 12"), hex("ff 80"), hex("01 00 00 00 00")
    };
    for (const std::uint8_t version : {0, 1}) {
        for (const std::uint8_t pdu : {0xA0, 0xA1}) {
            for (const auto& oid : invalidOids) {
                require(!SnmpRejectSession::Handle(packet(version, pdu, binding(oid))),
                    "Malformed OID must be silently dropped");
            }
            const auto body = wireTlv(0x02, bytes({version})) + wireTlv(0x04, "public");
            for (const auto& contents : invalidIntegers) {
                for (size_t field = 0; field < 3; ++field) {
                    std::string fields[]{hex("02 01 12"), hex("02 01 00"), hex("02 01 00")};
                    fields[field] = wireTlv(0x02, contents);
                    const auto request = wireTlv(0x30, body + wireTlv(pdu,
                        fields[0] + fields[1] + fields[2] + wireTlv(0x30, binding(kDescrWire))));
                    require(!SnmpRejectSession::Handle(request),
                        "Malformed INTEGER must be silently dropped, field " + std::to_string(field));
                }
            }
        }
    }
}

} // namespace

int main() {
    try {
        runTest("fixed v1/v2c Get and GetNext responses", testFixedV1AndV2Responses);
        runTest("original TLV preservation", testOriginalTlvPreservation);
        runTest("malformed and unsupported requests", testMalformedAndUnsupportedRequests);
        runTest("message size boundary", testMessageSizeBoundary);
        runTest("invalid primitive contents", testInvalidPrimitiveContents);
    } catch (const std::exception& error) {
        std::cerr << "SNMP reject session tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
