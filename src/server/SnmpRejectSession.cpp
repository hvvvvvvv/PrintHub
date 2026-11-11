#include <SnmpRejectSession.h>
#include <SnmpBer.h>

namespace {

using snmp::ber::EncodeTlv;
using snmp::ber::IsValidIntegerContents;
using snmp::ber::IsValidOidContents;
using snmp::ber::Reader;
using snmp::ber::Tlv;

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

bool isVersion(const Tlv& version, int expected) {
    return version.tag == kInteger && version.value.size() == 1
        && static_cast<unsigned char>(version.value.front()) == static_cast<unsigned char>(expected);
}

bool parseVarBinds(std::string_view requestVarBinds, bool getNext, std::string* responseVarBinds) {
    Reader list(requestVarBinds);
    while (!list.Done()) {
        Tlv requestVarBind{};
        if (!list.Read(requestVarBind) || requestVarBind.tag != kSequence) {
            return false;
        }

        Reader fields(requestVarBind.value);
        Tlv objectIdentifier{};
        Tlv value{};
        if (!fields.ReadRaw(objectIdentifier)
            || objectIdentifier.tag != kObjectIdentifier
            || !IsValidOidContents(objectIdentifier.value)
            || !fields.Read(value)
            || value.tag != 0x05 || !value.value.empty()
            || !fields.Done()) {
            return false;
        }

        if (responseVarBinds != nullptr) {
            const auto exceptionTag = getNext ? kEndOfMibView : kNoSuchObject;
            std::string responseVarBindValue(objectIdentifier.raw);
            responseVarBindValue += EncodeTlv(exceptionTag, {});
            *responseVarBinds += EncodeTlv(kSequence, responseVarBindValue);
        }
    }
    return true;
}

} // namespace

std::optional<std::string> SnmpRejectSession::Handle(std::string_view request) {
    if (request.empty() || request.size() > MAX_MESSAGE_SIZE_BYTES) {
        return std::nullopt;
    }

    Reader outer(request);
    Tlv message{};
    if (!outer.Read(message) || message.tag != kSequence || !outer.Done()) {
        return std::nullopt;
    }

    Reader body(message.value);
    Tlv version{};
    Tlv community{};
    Tlv requestPdu{};
    if (!body.ReadRaw(version)
        || !body.ReadRaw(community)
        || !body.Read(requestPdu)
        || !body.Done()
        || (!isVersion(version, 0) && !isVersion(version, 1))
        || community.tag != kOctetString || community.value != kCommunity
        || (requestPdu.tag != kGetRequest && requestPdu.tag != kGetNextRequest)) {
        return std::nullopt;
    }

    Reader pdu(requestPdu.value);
    Tlv requestId{};
    Tlv requestErrorStatus{};
    Tlv requestErrorIndex{};
    Tlv requestVarBinds{};
    if (!pdu.ReadRaw(requestId)
        || !pdu.Read(requestErrorStatus)
        || !pdu.Read(requestErrorIndex)
        || !pdu.ReadRaw(requestVarBinds)
        || !pdu.Done()
        || requestId.tag != kInteger
        || !IsValidIntegerContents(requestId.value)
        || requestErrorStatus.tag != kInteger
        || !IsValidIntegerContents(requestErrorStatus.value)
        || requestErrorIndex.tag != kInteger
        || !IsValidIntegerContents(requestErrorIndex.value)
        || requestVarBinds.tag != kSequence
        || requestVarBinds.value.empty()) {
        return std::nullopt;
    }

    const bool isV1 = isVersion(version, 0);
    std::string responseVarBinds;
    // v1 only validates; v2c builds exceptions during the same traversal.
    if (!parseVarBinds(requestVarBinds.value, requestPdu.tag == kGetNextRequest,
        isV1 ? nullptr : &responseVarBinds)) {
        return std::nullopt;
    }

    std::string responsePduValue = std::string(requestId.raw);
    if (isV1) {
        responsePduValue += EncodeTlv(kInteger, std::string_view("\x02", 1));
        responsePduValue += EncodeTlv(kInteger, std::string_view("\x01", 1));
        responsePduValue += requestVarBinds.raw;
    }
    else {
        responsePduValue += EncodeTlv(kInteger, std::string_view("\x00", 1));
        responsePduValue += EncodeTlv(kInteger, std::string_view("\x00", 1));
        responsePduValue += EncodeTlv(kSequence, responseVarBinds);
    }

    std::string responseMessage(version.raw);
    responseMessage += community.raw;
    responseMessage += EncodeTlv(kGetResponse, responsePduValue);
    return EncodeTlv(kSequence, responseMessage);
}
