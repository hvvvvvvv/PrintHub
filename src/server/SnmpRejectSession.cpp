#include <SnmpRejectSession.h>

#include <cstdint>
#include <limits>

namespace {

constexpr unsigned char kSequence = 0x30;
constexpr unsigned char kInteger = 0x02;
constexpr unsigned char kOctetString = 0x04;
constexpr unsigned char kObjectIdentifier = 0x06;
constexpr unsigned char kGetRequest = 0xA0;
constexpr unsigned char kGetNextRequest = 0xA1;
constexpr unsigned char kGetResponse = 0xA2;
constexpr unsigned char kNoSuchObject = 0x80;
constexpr unsigned char kEndOfMibView = 0x82;
constexpr std::string_view kCommunity = "public";

struct Tlv {
    unsigned char tag;
    std::string_view raw;
    std::string_view value;
};

bool readTlv(std::string_view input, size_t& offset, Tlv& result) {
    if (offset >= input.size()) {
        return false;
    }

    const auto begin = offset;
    result.tag = static_cast<unsigned char>(input[offset++]);
    if (offset >= input.size()) {
        return false;
    }

    const auto firstLengthByte = static_cast<unsigned char>(input[offset++]);
    size_t length = 0;
    if ((firstLengthByte & 0x80U) == 0) {
        length = firstLengthByte;
    }
    else {
        const auto lengthBytes = firstLengthByte & 0x7FU;
        if (lengthBytes == 0 || lengthBytes > sizeof(size_t) || offset + lengthBytes > input.size()) {
            return false;
        }
        for (unsigned char index = 0; index < lengthBytes; ++index) {
            const auto byte = static_cast<unsigned char>(input[offset++]);
            if (length > ((std::numeric_limits<size_t>::max)() >> 8U)) {
                return false;
            }
            length = (length << 8U) | byte;
        }
    }

    if (length > input.size() - offset) {
        return false;
    }
    result.value = input.substr(offset, length);
    offset += length;
    result.raw = input.substr(begin, offset - begin);
    return true;
}

bool isVersion(const Tlv& version, int expected) {
    return version.tag == kInteger && version.value.size() == 1
        && static_cast<unsigned char>(version.value.front()) == static_cast<unsigned char>(expected);
}

void appendLength(std::string& output, size_t length) {
    if (length < 0x80U) {
        output.push_back(static_cast<char>(length));
        return;
    }

    unsigned char bytes[sizeof(size_t)]{};
    size_t byteCount = 0;
    while (length != 0) {
        bytes[byteCount++] = static_cast<unsigned char>(length & 0xFFU);
        length >>= 8U;
    }
    output.push_back(static_cast<char>(0x80U | byteCount));
    while (byteCount != 0) {
        output.push_back(static_cast<char>(bytes[--byteCount]));
    }
}

std::string makeTlv(unsigned char tag, std::string_view value) {
    std::string result;
    result.reserve(value.size() + sizeof(size_t) + 2);
    result.push_back(static_cast<char>(tag));
    appendLength(result, value.size());
    result.append(value);
    return result;
}

bool parseVarBinds(std::string_view requestVarBinds, bool getNext, std::string* responseVarBinds) {
    size_t listOffset = 0;
    while (listOffset < requestVarBinds.size()) {
        Tlv requestVarBind{};
        if (!readTlv(requestVarBinds, listOffset, requestVarBind) || requestVarBind.tag != kSequence) {
            return false;
        }

        size_t varBindOffset = 0;
        Tlv objectIdentifier{};
        Tlv value{};
        if (!readTlv(requestVarBind.value, varBindOffset, objectIdentifier)
            || objectIdentifier.tag != kObjectIdentifier
            || !readTlv(requestVarBind.value, varBindOffset, value)
            || value.tag != 0x05 || !value.value.empty()
            || varBindOffset != requestVarBind.value.size()) {
            return false;
        }

        if (responseVarBinds != nullptr) {
            const auto exceptionTag = getNext ? kEndOfMibView : kNoSuchObject;
            std::string responseVarBindValue(objectIdentifier.raw);
            responseVarBindValue += makeTlv(exceptionTag, {});
            *responseVarBinds += makeTlv(kSequence, responseVarBindValue);
        }
    }
    return true;
}

} // namespace

std::optional<std::string> SnmpRejectSession::Handle(std::string_view request) {
    if (request.empty() || request.size() > MAX_MESSAGE_SIZE_BYTES) {
        return std::nullopt;
    }

    size_t outerOffset = 0;
    Tlv message{};
    if (!readTlv(request, outerOffset, message) || message.tag != kSequence || outerOffset != request.size()) {
        return std::nullopt;
    }

    size_t messageOffset = 0;
    Tlv version{};
    Tlv community{};
    Tlv requestPdu{};
    if (!readTlv(message.value, messageOffset, version)
        || !readTlv(message.value, messageOffset, community)
        || !readTlv(message.value, messageOffset, requestPdu)
        || messageOffset != message.value.size()
        || (!isVersion(version, 0) && !isVersion(version, 1))
        || community.tag != kOctetString || community.value != kCommunity
        || (requestPdu.tag != kGetRequest && requestPdu.tag != kGetNextRequest)) {
        return std::nullopt;
    }

    size_t pduOffset = 0;
    Tlv requestId{};
    Tlv requestErrorStatus{};
    Tlv requestErrorIndex{};
    Tlv requestVarBinds{};
    if (!readTlv(requestPdu.value, pduOffset, requestId)
        || !readTlv(requestPdu.value, pduOffset, requestErrorStatus)
        || !readTlv(requestPdu.value, pduOffset, requestErrorIndex)
        || !readTlv(requestPdu.value, pduOffset, requestVarBinds)
        || pduOffset != requestPdu.value.size()
        || requestId.tag != kInteger
        || requestErrorStatus.tag != kInteger
        || requestErrorIndex.tag != kInteger
        || requestVarBinds.tag != kSequence
        || requestVarBinds.value.empty()
        || !parseVarBinds(requestVarBinds.value, requestPdu.tag == kGetNextRequest, nullptr)) {
        return std::nullopt;
    }

    std::string responsePduValue = std::string(requestId.raw);
    if (isVersion(version, 0)) {
        responsePduValue += makeTlv(kInteger, std::string_view("\x02", 1));
        responsePduValue += makeTlv(kInteger, std::string_view("\x01", 1));
        responsePduValue += requestVarBinds.raw;
    }
    else {
        std::string responseVarBinds;
        if (!parseVarBinds(requestVarBinds.value, requestPdu.tag == kGetNextRequest, &responseVarBinds)) {
            return std::nullopt;
        }
        responsePduValue += makeTlv(kInteger, std::string_view("\x00", 1));
        responsePduValue += makeTlv(kInteger, std::string_view("\x00", 1));
        responsePduValue += makeTlv(kSequence, responseVarBinds);
    }

    std::string responseMessage(version.raw);
    responseMessage += community.raw;
    responseMessage += makeTlv(kGetResponse, responsePduValue);
    return makeTlv(kSequence, responseMessage);
}
