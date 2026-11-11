#include <SnmpBer.h>

#include "TestSupport.h"

#include <limits>
#include <vector>

namespace {

using snmp::ber::Reader;
using snmp::ber::Tlv;

void testIntegerContentsValidation() {
    for (unsigned int value = 0; value <= 0xFF; ++value) {
        require(snmp::ber::IsValidIntegerContents(bytes({static_cast<std::uint8_t>(value)})),
            "Every single-byte signed INTEGER must be valid");
    }
    const std::vector<std::string> valid{
        bytes({0x00, 0x80}), bytes({0x00, 0xFF}), bytes({0x01, 0x00}),
        bytes({0x7F, 0xFF}), bytes({0x80, 0x00}), bytes({0xFF, 0x7F}), bytes({0xFF, 0x00}),
        bytes({0x00, 0x80, 0x00}), bytes({0xFF, 0x7F, 0xFF}),
        bytes({0x7F, 0xFF, 0xFF, 0xFF}), bytes({0x80, 0x00, 0x00, 0x00}),
        bytes({0x00, 0x80, 0x00, 0x00}), bytes({0xFF, 0x7F, 0xFF, 0xFF})
    };
    for (size_t index = 0; index < valid.size(); ++index) {
        require(snmp::ber::IsValidIntegerContents(valid[index]),
            "Valid minimal INTEGER rejected, case " + std::to_string(index));
    }
    const std::vector<std::string> invalid{
        "", bytes({0x00, 0x00}), bytes({0x00, 0x12}), bytes({0x00, 0x7F}),
        bytes({0xFF, 0x80}), bytes({0xFF, 0xFF}),
        bytes({0x00, 0x00, 0x80}), bytes({0xFF, 0xFF, 0x7F}),
        bytes({0x00, 0x7F, 0xFF, 0xFF}), bytes({0xFF, 0x80, 0x00, 0x00}),
        bytes({0x01, 0x00, 0x00, 0x00, 0x00}), bytes({0x00, 0x80, 0x00, 0x00, 0x00}),
        bytes({0xFF, 0x7F, 0xFF, 0xFF, 0xFF})
    };
    for (size_t index = 0; index < invalid.size(); ++index) {
        require(!snmp::ber::IsValidIntegerContents(invalid[index]),
            "Invalid INTEGER accepted, case " + std::to_string(index));
    }
}

void testOidContentsValidation() {
    const std::vector<std::string> valid{
        bytes({0x00}), bytes({0x2B}), bytes({0x7F}), bytes({0x81, 0x00}), bytes({0xFF, 0x7F}),
        bytes({0x81, 0x80, 0x00}), bytes({0x2B, 0x00}), bytes({0x2B, 0x81, 0x00}),
        bytes({0x2B, 0x81, 0x80, 0x00, 0x00, 0xFF, 0x7F}),
        bytes({0x2B, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00})
    };
    for (size_t index = 0; index < valid.size(); ++index) {
        require(snmp::ber::IsValidOidContents(valid[index]),
            "Valid base-128 OID rejected, case " + std::to_string(index));
    }
    const std::vector<std::string> invalid{
        "", bytes({0x80}), bytes({0x81}), bytes({0xFF}), bytes({0x81, 0x80}),
        bytes({0x2B, 0x81}), bytes({0x2B, 0x81, 0x80}),
        bytes({0x80, 0x00}), bytes({0x80, 0x01}), bytes({0x2B, 0x80, 0x01}),
        bytes({0x2B, 0x80, 0x81, 0x00}), bytes({0x81, 0x00, 0x80, 0x01})
    };
    for (size_t index = 0; index < invalid.size(); ++index) {
        require(!snmp::ber::IsValidOidContents(invalid[index]),
            "Invalid base-128 OID accepted, case " + std::to_string(index));
    }
}

void testLengthEncodingAndRoundTrip() {
    struct Example {
        size_t length;
        std::string encodedLength;
    };
    const std::vector<Example> examples{
        {0, bytes({0})}, {1, bytes({1})}, {127, bytes({0x7F})},
        {128, bytes({0x81, 0x80})}, {255, bytes({0x81, 0xFF})},
        {256, bytes({0x82, 1, 0})}, {65535, bytes({0x82, 0xFF, 0xFF})},
        {65536, bytes({0x83, 1, 0, 0})}
    };
    for (const auto& example : examples) {
        runTest("payload length " + std::to_string(example.length), [&] {
            std::string appended = "prefix";
            snmp::ber::AppendLength(appended, example.length);
            require(appended == "prefix" + example.encodedLength, "AppendLength bytes or existing prefix changed");
            std::string payload(example.length, '\xA5');
            if (!payload.empty()) payload[payload.size() / 2] = '\0';
            const auto encoded = snmp::ber::EncodeTlv(0x04, payload);
            const auto expected = bytes({0x04}) + example.encodedLength + payload;
            require(encoded == expected, "EncodeTlv exact bytes");
            require(snmp::ber::EncodedSize(example.length) == expected.size(), "EncodedSize includes tag and length");
            Reader reader(encoded);
            Tlv tlv;
            require(!reader.Done() && reader.Read(tlv), "Encoded TLV must be readable");
            require(tlv.tag == 0x04 && tlv.value == payload && tlv.raw.empty(), "Decoded tag, value and opt-in raw");
            require(reader.Done() && !reader.Read(tlv), "Reader must stop exactly at end");
        });
    }
    // Exercise machine-width length encoding without allocating a huge payload.
    std::string maximum;
    snmp::ber::AppendLength(maximum, (std::numeric_limits<size_t>::max)());
    require(maximum == std::string(1, static_cast<char>(0x80U | sizeof(size_t))) +
        std::string(sizeof(size_t), '\xFF'), "SIZE_MAX length header must respect x86/x64 width");
}

void testReaderSequenceAndRawCapture() {
    const auto first = bytes({0x02, 1, 0x7F});
    const auto second = bytes({0x04, 0x81, 3}) + "abc"; // Accepted non-minimal length form.
    const auto third = bytes({0x05, 0});
    const auto wire = first + second + third;
    Reader reader(wire);
    Tlv tlv;
    require(reader.ReadRaw(tlv) && tlv.tag == 2 && tlv.value == bytes({0x7F}) && tlv.raw == first,
        "ReadRaw first item");
    require(!reader.Done(), "More TLVs remain");
    require(reader.ReadRaw(tlv) && tlv.tag == 4 && tlv.value == "abc" && tlv.raw == second,
        "ReadRaw must preserve original long-form header");
    require(tlv.value.data() == wire.data() + first.size() + 3, "Reader must view original storage");
    require(!reader.Done() && reader.Read(tlv) && tlv.tag == 5 && tlv.value.empty() && tlv.raw.empty(),
        "Read must reset previous raw capture");
    require(reader.Done(), "All three TLVs consumed");
    require(!reader.ReadRaw(tlv) && reader.Done(), "Reading at EOF");

    const auto nested = bytes({0x30, 2, 0x05, 0});
    Reader nestedReader(nested);
    require(nestedReader.Read(tlv) && tlv.tag == 0x30 && tlv.value == bytes({5, 0}),
        "Constructed values are returned without recursively parsing");
    Reader inner(tlv.value);
    Tlv child;
    require(inner.Read(child) && child.tag == 5 && inner.Done(), "Explicit nested reader");
}

void testMalformedAndTruncatedInputs() {
    std::vector<std::string> malformed{
        "", bytes({0x04}), bytes({0x04, 1}), bytes({0x04, 2, 0}),
        bytes({0x04, 0x80, 0, 0}), // Indefinite length.
        bytes({0x04, 0x81}), bytes({0x04, 0x82, 1}),
        bytes({0x04, 0x81, 0x80}), bytes({0x04, 0x82, 1, 0, 0}),
        bytes({0x1F, 0}), bytes({0x3F, 0}), bytes({0x9F, 1, 0}),
        bytes({0x04, 0xFF}) // Length-of-length exceeds either machine width.
    };
    malformed.push_back(bytes({0x04}) +
        std::string(1, static_cast<char>(0x80U | (sizeof(size_t) + 1))) +
        std::string(sizeof(size_t) + 1, '\0'));
    malformed.push_back(bytes({0x04}) +
        std::string(1, static_cast<char>(0x80U | sizeof(size_t))) +
        std::string(sizeof(size_t), '\xFF'));
    for (size_t index = 0; index < malformed.size(); ++index) {
        Reader reader(malformed[index]);
        Tlv tlv{0x55, "unchanged", "raw"};
        require(!reader.ReadRaw(tlv), "Malformed TLV accepted: " + std::to_string(index));
        require(tlv.tag == 0x55 && tlv.value == "unchanged" && tlv.raw == "raw", "Failed read modified output");
    }
    const auto complete = bytes({0x04, 0x82, 1, 0}) + std::string(256, 'x');
    for (size_t length = 0; length < complete.size(); ++length) {
        Reader reader(std::string_view(complete).substr(0, length));
        Tlv tlv;
        require(!reader.Read(tlv), "Truncated long TLV accepted");
    }
}

void testDoneDescribesCursorNotValidity() {
    Reader empty("");
    Tlv tlv;
    require(empty.Done() && !empty.Read(tlv), "Empty reader is done");
    const auto oneByte = bytes({0x04});
    Reader incompleteHeader(oneByte);
    require(!incompleteHeader.Read(tlv) && !incompleteHeader.Done(), "Unconsumed short header");
    const auto missingPayload = bytes({0x04, 1});
    Reader incompletePayload(missingPayload);
    require(!incompletePayload.Read(tlv) && incompletePayload.Done(),
        "Current reader consumes a complete header before rejecting a missing payload");
    const auto trailing = bytes({0x05, 0, 0x04});
    Reader trailingReader(trailing);
    require(trailingReader.Read(tlv) && !trailingReader.Done() && !trailingReader.Read(tlv) && !trailingReader.Done(),
        "Trailing byte must remain visible to Done");
}

} // namespace

int main() {
    try {
        runTest("INTEGER contents validation", testIntegerContentsValidation);
        runTest("OID contents validation", testOidContentsValidation);
        runTest("length encoding and round trip", testLengthEncodingAndRoundTrip);
        runTest("sequential reads and raw capture", testReaderSequenceAndRawCapture);
        runTest("malformed and truncated inputs", testMalformedAndTruncatedInputs);
        runTest("Done semantics", testDoneDescribesCursorNotValidity);
    } catch (const std::exception& error) {
        std::cerr << "SNMP BER tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
