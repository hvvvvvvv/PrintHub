#include <LpdSession.h>
#include <RawPrintSession.h>
#include <SnmpRejectSession.h>
#include <ServiceConfiguration.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr char kAck = 0x00;
constexpr char kNak = 0x01;
constexpr char kReceiveJob = 0x02;
constexpr char kReceiveControlFile = 0x02;
constexpr char kReceiveDataFile = 0x03;
constexpr char kAbortJob = 0x01;
constexpr char kSequence = 0x30;
constexpr char kInteger = 0x02;
constexpr char kOctetString = 0x04;
constexpr char kObjectIdentifier = 0x06;
constexpr char kNull = 0x05;
constexpr char kGetRequest = static_cast<char>(0xA0);
constexpr char kGetNextRequest = static_cast<char>(0xA1);
constexpr char kGetResponse = static_cast<char>(0xA2);
constexpr char kNoSuchObject = static_cast<char>(0x80);
constexpr char kEndOfMibView = static_cast<char>(0x82);

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string tlv(char tag, const std::string& value) {
    require(value.size() < 128, "test BER value is unexpectedly large");
    return std::string(1, tag) + static_cast<char>(value.size()) + value;
}

std::string snmpRequest(char version, char pdu, const std::string& community, const std::string& varBinds, char requestId = 0x12) {
    const auto pduValue = tlv(kInteger, std::string(1, requestId)) + tlv(kInteger, std::string(1, 0))
        + tlv(kInteger, std::string(1, 0)) + tlv(kSequence, varBinds);
    return tlv(kSequence, tlv(kInteger, std::string(1, version)) + tlv(kOctetString, community) + tlv(pdu, pduValue));
}

std::string varBind(const std::string& oid) {
    return tlv(kSequence, tlv(kObjectIdentifier, oid) + tlv(kNull, ""));
}

void testSnmpRejectSession() {
    const std::string sysDescrOid("\x2b\x06\x01\x02\x01\x01\x01\x00", 8);
    const std::string sysNameOid("\x2b\x06\x01\x02\x01\x01\x05\x00", 8);
    const auto requestVarBinds = varBind(sysDescrOid) + varBind(sysNameOid);

    const auto v1Request = snmpRequest(0, kGetRequest, "public", requestVarBinds, 0x22);
    const auto v1Response = SnmpRejectSession::Handle(v1Request);
    require(v1Response.has_value(), "SNMP v1 request was not answered");
    const auto expectedV1Pdu = tlv(kInteger, std::string(1, 0x22)) + tlv(kInteger, std::string(1, 2))
        + tlv(kInteger, std::string(1, 1)) + tlv(kSequence, requestVarBinds);
    require(*v1Response == tlv(kSequence, tlv(kInteger, std::string(1, 0)) + tlv(kOctetString, "public")
        + tlv(kGetResponse, expectedV1Pdu)), "SNMP v1 rejection response changed");

    const auto v2Request = snmpRequest(1, kGetRequest, "public", requestVarBinds, 0x33);
    const auto v2Response = SnmpRejectSession::Handle(v2Request);
    require(v2Response.has_value(), "SNMP v2c GetRequest was not answered");
    const auto v2VarBinds = tlv(kSequence, tlv(kObjectIdentifier, sysDescrOid) + tlv(kNoSuchObject, ""))
        + tlv(kSequence, tlv(kObjectIdentifier, sysNameOid) + tlv(kNoSuchObject, ""));
    const auto expectedV2Pdu = tlv(kInteger, std::string(1, 0x33)) + tlv(kInteger, std::string(1, 0))
        + tlv(kInteger, std::string(1, 0)) + tlv(kSequence, v2VarBinds);
    require(*v2Response == tlv(kSequence, tlv(kInteger, std::string(1, 1)) + tlv(kOctetString, "public")
        + tlv(kGetResponse, expectedV2Pdu)), "SNMP v2c GetRequest response changed");

    const auto getNextResponse = SnmpRejectSession::Handle(snmpRequest(1, kGetNextRequest, "public", varBind(sysDescrOid)));
    require(getNextResponse.has_value() && getNextResponse->find(kEndOfMibView) != std::string::npos,
        "SNMP v2c GetNext did not return endOfMibView");
    require(!SnmpRejectSession::Handle(snmpRequest(1, kGetRequest, "private", varBind(sysDescrOid))).has_value(),
        "unknown SNMP community was answered");
    require(!SnmpRejectSession::Handle("\x30\x02\x02").has_value(), "truncated BER request was answered");
    require(!SnmpRejectSession::Handle(snmpRequest(0, kGetRequest, "public",
        tlv(kSequence, tlv(kObjectIdentifier, sysDescrOid) + tlv(kNull, "unexpected")))).has_value(),
        "malformed SNMP VarBind was answered");
    require(!SnmpRejectSession::Handle(snmpRequest(1, static_cast<char>(0xA3), "public", varBind(sysDescrOid))).has_value(),
        "unsupported SNMP PDU was answered");
    require(!SnmpRejectSession::Handle(std::string(SnmpRejectSession::MAX_MESSAGE_SIZE_BYTES + 1, '\0')).has_value(),
        "oversized SNMP request was answered");
}

class FakeLpdTransport final : public ILpdTransport {
public:
    explicit FakeLpdTransport(std::string input, size_t chunkSize = 2)
        : m_input(std::move(input)), m_chunkSize(chunkSize) {}

    LpdEndpoint Endpoint() const override { return { "127.0.0.1", 721 }; }
    void WriteByte(char value) override { responses += value; }

    void ReadLine(std::string& line) override {
        const auto newline = m_input.find('\n', m_position);
        if (newline == std::string::npos) {
            throw LpdTransportDisconnected(Endpoint());
        }
        line.assign(m_input.data() + m_position, newline - m_position);
        m_position = newline + 1;
    }

    void ReadExactly(size_t size, const BufferCallback& callback) override {
        ++readExactlyCalls;
        size_t remaining = size;
        while (remaining != 0) {
            if (m_position >= m_input.size()) {
                throw LpdTransportDisconnected(Endpoint());
            }
            const auto available = (std::min)(remaining, (std::min)(m_chunkSize, m_input.size() - m_position));
            if (available == 0) {
                throw LpdTransportDisconnected(Endpoint());
            }
            callback(m_input.data() + m_position, available);
            m_position += available;
            remaining -= available;
        }
    }

    void ReadToEnd(const BufferCallback& callback) override {
        while (m_position < m_input.size()) {
            const auto available = (std::min)(m_chunkSize, m_input.size() - m_position);
            callback(m_input.data() + m_position, available);
            m_position += available;
        }
    }

    std::string responses;
    size_t readExactlyCalls = 0;

private:
    std::string m_input;
    size_t m_position = 0;
    size_t m_chunkSize;
};

struct FakeJobState {
    std::string documentName;
    std::string data;
    bool committed = false;
    bool canceled = false;
};

class FakePrintJob final : public IPrintJob {
public:
    explicit FakePrintJob(std::shared_ptr<FakeJobState> state) : m_state(std::move(state)) {}
    ~FakePrintJob() override {
        if (!m_state->committed) {
            m_state->canceled = true;
        }
    }
    void Write(const char* data, size_t size) override { m_state->data.append(data, size); }
    void SetDocumentName(const std::string& name) override { m_state->documentName = name; }
    void Cancel() override { m_state->canceled = true; }
    void Commit() override { m_state->committed = true; }
    size_t GetBytesWritten() const override { return m_state->data.size(); }

private:
    std::shared_ptr<FakeJobState> m_state;
};

class FakeFactory final : public IPrinterJobFactory {
public:
    std::unique_ptr<IPrintJob> Create(const std::string&, const std::string& documentName) override {
        auto state = std::make_shared<FakeJobState>();
        state->documentName = documentName;
        jobs.push_back(state);
        return std::make_unique<FakePrintJob>(std::move(state));
    }

    std::vector<std::shared_ptr<FakeJobState>> jobs;
};

class FakeRawTransport final : public IRawPrintTransport {
public:
    explicit FakeRawTransport(std::string data) : m_data(std::move(data)) {}
    void ReadToEnd(const BufferCallback& callback) override {
        const auto firstSize = m_data.size() / 2;
        if (firstSize != 0) {
            callback(m_data.data(), firstSize);
        }
        if (firstSize != m_data.size()) {
            callback(m_data.data() + firstSize, m_data.size() - firstSize);
        }
    }

private:
    std::string m_data;
};

std::string receiveJobPrefix() {
    return std::string(1, kReceiveJob) + "queue\n";
}

std::string fileCommand(char command, size_t size, const std::string& name) {
    return std::string(1, command) + std::to_string(size) + " " + name + "\n";
}

void testExactCountControlFirst() {
    const std::string control = "JWindows job\nfdfA\n";
    const std::string data{ 'a', '\0', 'b' };
    auto input = receiveJobPrefix() + fileCommand(kReceiveControlFile, control.size(), "cfA") + control + '\0'
        + fileCommand(kReceiveDataFile, data.size(), "dfA") + data + '\0';
    FakeLpdTransport transport(std::move(input));
    FakeFactory factory;
    LpdSession session(factory, [](const std::string&) { return std::optional<std::string>("Printer"); }, control.size());
    session.Run(transport);

    require(factory.jobs.size() == 1, "exact-count job was not created");
    require(factory.jobs.front()->committed, "exact-count job was not committed");
    require(factory.jobs.front()->documentName == "Windows job", "J control did not update document name");
    require(factory.jobs.front()->data == data, "binary data was changed");
    require(transport.responses == std::string(5, kAck), "exact-count ACK sequence changed");
}

void testOversizedControlFileIsRejectedBeforeRead() {
    FakeLpdTransport transport(receiveJobPrefix() + fileCommand(kReceiveControlFile, 5, "cfA"));
    FakeFactory factory;
    LpdSession session(factory, [](const std::string&) { return std::optional<std::string>("Printer"); }, 4);
    session.Run(transport);

    require(transport.responses == std::string{ kAck, kNak }, "oversized LPD control file did not return NAK");
    require(transport.readExactlyCalls == 0, "oversized LPD control file content was read");
    require(factory.jobs.size() == 1 && !factory.jobs.front()->committed && factory.jobs.front()->canceled,
        "oversized LPD control file left a submitted print job");
}

void testDataFirstAndCountZero() {
    const std::string data = "abc";
    const std::string control = "fdfA\n";
    auto dataFirst = receiveJobPrefix() + fileCommand(kReceiveDataFile, data.size(), "dfA") + data + '\0'
        + fileCommand(kReceiveControlFile, control.size(), "cfA") + control + '\0';
    FakeLpdTransport dataFirstTransport(std::move(dataFirst), 1);
    FakeFactory dataFirstFactory;
    LpdSession dataFirstSession(dataFirstFactory, [](const std::string&) { return std::optional<std::string>("Printer"); });
    dataFirstSession.Run(dataFirstTransport);
    require(dataFirstFactory.jobs.front()->committed, "data-first counted job was not committed");

    auto zeroCount = receiveJobPrefix() + fileCommand(kReceiveControlFile, control.size(), "cfA") + control + '\0'
        + fileCommand(kReceiveDataFile, 0, "dfA") + data;
    FakeLpdTransport zeroCountTransport(std::move(zeroCount));
    FakeFactory zeroCountFactory;
    LpdSession zeroCountSession(zeroCountFactory, [](const std::string&) { return std::optional<std::string>("Printer"); });
    zeroCountSession.Run(zeroCountTransport);
    require(zeroCountFactory.jobs.front()->committed, "Count=0 job was not committed at EOF");
    require(zeroCountFactory.jobs.front()->data == data, "Count=0 data was changed");
    require(zeroCountTransport.responses == std::string(4, kAck), "Count=0 ACK sequence changed");
}

void testWindowsPlaceholderAndInvalidTerminator() {
    const std::string control = "fdfA\n";
    const std::string data = "placeholder-data";
    auto placeholder = receiveJobPrefix() + fileCommand(kReceiveControlFile, control.size(), "cfA") + control + '\0'
        + fileCommand(kReceiveDataFile, 99999, "dfA") + data;
    FakeLpdTransport placeholderTransport(std::move(placeholder), 3);
    FakeFactory placeholderFactory;
    LpdSession placeholderSession(placeholderFactory, [](const std::string&) { return std::optional<std::string>("Printer"); });
    placeholderSession.Run(placeholderTransport);
    require(placeholderFactory.jobs.front()->committed, "placeholder-count job was not committed at EOF");
    require(placeholderFactory.jobs.front()->data == data, "placeholder-count data was changed");

    auto malformed = receiveJobPrefix() + fileCommand(kReceiveControlFile, control.size(), "cfA") + control + 'x';
    FakeLpdTransport malformedTransport(std::move(malformed));
    FakeFactory malformedFactory;
    LpdSession malformedSession(malformedFactory, [](const std::string&) { return std::optional<std::string>("Printer"); });
    malformedSession.Run(malformedTransport);
    require(!malformedFactory.jobs.front()->committed, "invalid terminator committed a job");
    require(malformedFactory.jobs.front()->canceled, "invalid terminator did not cancel its job");
    require(!malformedTransport.responses.empty() && malformedTransport.responses.back() == kNak,
        "invalid terminator did not produce NAK");
}

void testMultipleFilesAbortAndUnknownQueue() {
    const std::string control = "fdfA\nfdfB\n";
    const std::string firstData = "first";
    const std::string secondData = "second";
    auto input = receiveJobPrefix() + fileCommand(kReceiveControlFile, control.size(), "cfA") + control + '\0'
        + fileCommand(kReceiveDataFile, firstData.size(), "dfA") + firstData + '\0'
        + fileCommand(kReceiveDataFile, secondData.size(), "dfB") + secondData + '\0';
    FakeLpdTransport transport(std::move(input));
    FakeFactory factory;
    LpdSession session(factory, [](const std::string&) { return std::optional<std::string>("Printer"); });
    session.Run(transport);
    require(factory.jobs.front()->committed, "multi-file job was not committed");
    require(factory.jobs.front()->data == firstData + secondData, "multi-file data was changed");

    const std::string replacementControl = "fdfB\n";
    auto abortInput = receiveJobPrefix() + fileCommand(kReceiveDataFile, firstData.size(), "dfA") + firstData + '\0'
        + std::string(1, kAbortJob) + "\n"
        + fileCommand(kReceiveControlFile, replacementControl.size(), "cfB") + replacementControl + '\0'
        + fileCommand(kReceiveDataFile, secondData.size(), "dfB") + secondData + '\0';
    FakeLpdTransport abortTransport(std::move(abortInput));
    FakeFactory abortFactory;
    LpdSession abortSession(abortFactory, [](const std::string&) { return std::optional<std::string>("Printer"); });
    abortSession.Run(abortTransport);
    require(abortFactory.jobs.size() == 2, "abort did not create a replacement job");
    require(abortFactory.jobs.front()->canceled, "abort did not cancel the original job");
    require(abortFactory.jobs.back()->committed, "replacement job was not committed");

    FakeLpdTransport unknownQueueTransport(receiveJobPrefix());
    FakeFactory unknownQueueFactory;
    LpdSession unknownQueueSession(unknownQueueFactory, [](const std::string&) { return std::optional<std::string>{}; });
    unknownQueueSession.Run(unknownQueueTransport);
    require(unknownQueueFactory.jobs.empty(), "unknown queue created a print job");
    require(unknownQueueTransport.responses == std::string(1, kNak), "unknown queue did not produce NAK");
}

void testConfiguredQueueResolver() {
    const std::vector<LpdQueueMapping> mappings{ { "Sales", "Mapped Printer" } };
    FakeLpdTransport matchingTransport(std::string(1, kReceiveJob) + "sAlEs\n");
    FakeFactory matchingFactory;
    LpdSession matchingSession(matchingFactory, [&mappings](const std::string& queue) {
        return ResolveLpdQueuePrinter(mappings, queue);
    });
    matchingSession.Run(matchingTransport);
    require(matchingFactory.jobs.size() == 1, "configured queue did not create a print job");

    FakeLpdTransport unmappedTransport(std::string(1, kReceiveJob) + "Mapped Printer\n");
    FakeFactory unmappedFactory;
    LpdSession unmappedSession(unmappedFactory, [&mappings](const std::string& queue) {
        return ResolveLpdQueuePrinter(mappings, queue);
    });
    unmappedSession.Run(unmappedTransport);
    require(unmappedFactory.jobs.empty() && unmappedTransport.responses == std::string(1, kNak),
        "unmapped printer name was accepted as an LPD queue");
}

void testRawCommitAndEmptyCancel() {
    FakeFactory factory;
    RawPrintSession session(factory, "Printer");
    FakeRawTransport transport(std::string("raw\0data", 8));
    session.Run(transport);
    require(factory.jobs.front()->committed, "RAW data was not committed");
    require(factory.jobs.front()->data == std::string("raw\0data", 8), "RAW data was changed");

    FakeRawTransport emptyTransport("");
    session.Run(emptyTransport);
    require(factory.jobs.back()->canceled, "empty RAW job was not canceled");
}

} // namespace

int main() {
    try {
        testExactCountControlFirst();
        testOversizedControlFileIsRejectedBeforeRead();
        testDataFirstAndCountZero();
        testWindowsPlaceholderAndInvalidTerminator();
        testMultipleFilesAbortAndUnknownQueue();
        testConfiguredQueueResolver();
        testRawCommitAndEmptyCancel();
        testSnmpRejectSession();
    }
    catch (const std::exception& error) {
        std::cerr << "Protocol session test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
