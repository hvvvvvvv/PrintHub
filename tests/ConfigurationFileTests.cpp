#include <ConfigurationFile.hpp>

#include "TestSupport.h"

#include <atomic>
#include <filesystem>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>
#include <TextEncoding.h>
#include <Windows.h>

namespace {

using Json = nlohmann::json;

constexpr auto kFullConfiguration = R"({
    "logLevel": "DeBuG",
    "maxPendingClients": 512,
    "lpdServer": {"listenAddress": "127.0.0.2", "port": 1515},
    "minThreads": {"raw": 2, "lpd": 3},
    "printerMap": [
        {"name": "Office Printer", "lprQueue": "Main Queue",
         "rawServer": {"listenAddress": "127.0.0.1", "port": 9200}},
        {"name": "Office Printer", "lprQueue": "Archive"}
    ],
    "ppmDisabled": true
})";

void requireSame(const ServiceConfiguration& actual, const ServiceConfiguration& expected) {
    require(actual.logLevel == expected.logLevel, "Log level");
    require(actual.maxPendingClients == expected.maxPendingClients, "Pending client limit");
    require(actual.lpdServer.listenAddress == expected.lpdServer.listenAddress, "LPD address");
    require(actual.lpdServer.port == expected.lpdServer.port, "LPD port");
    require(actual.rawServerMinThreads == expected.rawServerMinThreads, "RAW threads");
    require(actual.lpdMinThreads == expected.lpdMinThreads, "LPD threads");
    require(actual.ppmDisabled == expected.ppmDisabled, "PPM flag");
    require(actual.printerMap.size() == expected.printerMap.size(), "Printer mapping count");
    for (size_t index = 0; index < actual.printerMap.size(); ++index) {
        const auto& left = actual.printerMap[index];
        const auto& right = expected.printerMap[index];
        require(left.name == right.name, "Printer name/order");
        require(left.lprQueue == right.lprQueue, "LPR queue/order");
        require(left.rawServer.has_value() == right.rawServer.has_value(), "Optional RAW listener");
        if (left.rawServer) {
            require(left.rawServer->listenAddress == right.rawServer->listenAddress, "RAW address");
            require(left.rawServer->port == right.rawServer->port, "RAW port");
        }
    }
}

void requireInvalid(std::string_view json, std::string_view field, std::string_view reason = {}) {
    try {
        ParseConfigurationJson(json, "test");
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        require(message.starts_with("test: "), "Error must include the source");
        require(message.find(field) != std::string::npos, "Error must include the field path");
        require(message.find(reason) != std::string::npos, "Error must explain the failure");
        return;
    }
    throw std::runtime_error("Invalid configuration accepted: " + std::string(json));
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::atomic_uint counter{0};
        // Some locked-down hosts deny writes to the system temp directory for
        // test binaries; the working directory (the CTest binary directory) is
        // an equivalent, writable location.
        std::vector<std::filesystem::path> roots;
        std::error_code error;
        const auto systemTemp = std::filesystem::temp_directory_path(error);
        if (!error) roots.push_back(systemTemp);
        roots.push_back(std::filesystem::current_path());
        for (const auto& root : roots) {
            for (unsigned int attempt = 0; attempt < 64; ++attempt) {
                path_ = root / ("PrintHubConfigTest-" + std::to_string(GetCurrentProcessId()) + "-" +
                    std::to_string(GetTickCount64()) + "-" + std::to_string(counter.fetch_add(1)));
                if (std::filesystem::create_directory(path_, error)) return;
                if (error) break;
            }
        }
        throw std::runtime_error("Cannot choose a unique test directory");
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class TestHandle {
public:
    explicit TestHandle(HANDLE value) : value_(value) {}
    TestHandle(const TestHandle&) = delete;
    TestHandle& operator=(const TestHandle&) = delete;
    ~TestHandle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }
    HANDLE get() const { return value_; }

private:
    HANDLE value_;
};

void writeFile(const std::filesystem::path& path, std::string_view content) {
    TestHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(file.get() != INVALID_HANDLE_VALUE, "Cannot create test file");
    size_t offset = 0;
    while (offset < content.size()) {
        DWORD count = 0;
        require(WriteFile(file.get(), content.data() + offset,
            static_cast<DWORD>(content.size() - offset), &count, nullptr) != FALSE, "Cannot write test file");
        require(count != 0, "Zero-byte test write");
        offset += static_cast<size_t>(count);
    }
}

std::filesystem::path temporarySavePath(const std::filesystem::path& path) {
    return std::filesystem::path(path.native() + L".tmp." + std::to_wstring(GetCurrentProcessId()));
}

void testDefaultsAndMinimalMapping() {
    requireSame(ParseConfigurationJson("{}", "test"), ServiceConfiguration{});
    ServiceConfiguration expected;
    expected.printerMap = {{"Printer", "Queue", std::nullopt}};
    const auto minimal = ParseConfigurationJson(R"({"printerMap":[{"name":"Printer","lprQueue":"Queue"}]})", "test");
    requireSame(minimal, expected);
    require(minimal.lpdServer.port == 515 && !minimal.printerMap[0].rawServer, "LPD-only defaults");
    expected.printerMap[0].rawServer.emplace();
    const auto raw = ParseConfigurationJson(R"({"printerMap":[{"name":"Printer","lprQueue":"Queue","rawServer":{}}]})", "test");
    requireSame(raw, expected);
    require(raw.printerMap[0].rawServer->listenAddress == "0.0.0.0" &&
        raw.printerMap[0].rawServer->port == 9100, "Omitted RAW fields must use defaults");
    requireSame(ParseConfigurationJson(R"({"lpdServer":{},"minThreads":{},"printerMap":[]})", "test"),
        ServiceConfiguration{});
    auto empty = ParseConfigurationJson(R"({"lpdServer":{"port":1515}})", "test");
    require(empty.printerMap.empty() && empty.lpdServer.port == 1515 && !empty.ppmDisabled,
        "An empty printer map does not restrict the LPD port");
}

void testFullConfiguration() {
    ServiceConfiguration expected;
    expected.logLevel = ServiceLogLevel::Debug;
    expected.maxPendingClients = 512;
    expected.lpdServer = {"127.0.0.2", 1515};
    expected.rawServerMinThreads = 2;
    expected.lpdMinThreads = 3;
    expected.printerMap = {
        {"Office Printer", "Main Queue", RawServerConfiguration{"127.0.0.1", 9200}},
        {"Office Printer", "Archive", std::nullopt},
    };
    expected.ppmDisabled = true;
    requireSame(ParseConfigurationJson(kFullConfiguration, "test"), expected);
}

void testSchemaErrors() {
    const std::vector<std::pair<std::string, std::string>> invalid{
        {R"({"printerMap":[{"lprQueue":"Queue"}]})", "printerMap[0].name"},
        {R"({"printerMap":[{"name":"Printer"}]})", "printerMap[0].lprQueue"},
        {R"({"printerMap":[{"name":"","lprQueue":"Queue"}]})", "printerMap[0].name"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":""}]})", "printerMap[0].lprQueue"},
        {R"({"unknown":1})", "unknown"},
        {R"({"listenAddress":"0.0.0.0"})", "listenAddress"},
        {R"({"version":1})", "version"},
        {R"({"maxControlFileBytes":1})", "maxControlFileBytes"},
        {R"({"lpdServer":{"extra":1}})", "lpdServer.extra"},
        {R"({"lpdServer":{"maxControlFileBytes":1}})", "lpdServer.maxControlFileBytes"},
        {R"({"minThreads":{"extra":1}})", "minThreads.extra"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":"Queue","extra":1}]})", "printerMap[0].extra"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":"Queue","rawServer":{"extra":1}}]})",
            "printerMap[0].rawServer.extra"},
    };
    for (const auto& [json, field] : invalid) {
        runTest(field, [&] { requireInvalid(json, field); });
    }
    for (const auto* invalidJson : {"", "{", R"({"logLevel":"info",})", "{} {}", R"({"x":1e10000})"}) {
        requireInvalid(invalidJson, "invalid JSON");
    }
    requireInvalid(std::string("{}\0{}", 5), "invalid JSON");
    for (const auto* root : {"[]", "null", "42", "true", R"("text")"}) {
        requireInvalid(root, "$", "expected object");
    }
}

void testTypeAndRangeErrors() {
    const std::vector<std::pair<std::string, std::string>> typeErrors{
        {R"({"logLevel":1})", "logLevel: expected string"},
        {R"({"lpdServer":[]})", "lpdServer: expected object"},
        {R"({"lpdServer":{"listenAddress":42}})", "lpdServer.listenAddress: expected string"},
        {R"({"minThreads":null})", "minThreads: expected object"},
        {R"({"ppmDisabled":"true"})", "ppmDisabled: expected boolean"},
        {R"({"ppmDisabled":0})", "ppmDisabled: expected boolean"},
        {R"({"printerMap":{}})", "printerMap: expected array"},
        {R"({"printerMap":[null]})", "printerMap[0]: expected object"},
        {R"({"printerMap":[{"name":false,"lprQueue":"Queue"}]})", "printerMap[0].name: expected string"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":null}]})", "printerMap[0].lprQueue: expected string"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":"Queue","rawServer":null}]})",
            "printerMap[0].rawServer: expected object"},
    };
    for (const auto& [json, message] : typeErrors) {
        runTest(message, [&] { requireInvalid(json, message); });
    }
    for (const auto* invalid : {"0", "-1", "65536", "18446744073709551615", "18446744073709551616",
        "1.0", "1e1", R"("515")", "null", "true", "[]", "{}"}) {
        const std::string token = invalid;
        requireInvalid(R"({"lpdServer":{"port":)" + token + "}}", "lpdServer.port", "expected integer");
        requireInvalid(R"({"maxPendingClients":)" + token + "}", "maxPendingClients", "expected integer");
        requireInvalid(R"({"minThreads":{"raw":)" + token + "}}", "minThreads.raw", "expected integer");
        requireInvalid(R"({"minThreads":{"lpd":)" + token + "}}", "minThreads.lpd", "expected integer");
        requireInvalid(R"({"printerMap":[{"name":"Printer","lprQueue":"Queue","rawServer":{"port":)" +
            token + "}}]}", "printerMap[0].rawServer.port", "expected integer");
    }
    for (const int boundary : {1, 65535}) {
        const auto token = std::to_string(boundary);
        const auto value = ParseConfigurationJson(R"({"ppmDisabled":true,"maxPendingClients":)" + token +
            R"(,"minThreads":{"raw":)" + token + R"(,"lpd":)" + token +
            R"(},"lpdServer":{"port":)" + token +
            R"(},"printerMap":[{"name":"Printer","lprQueue":"Queue","rawServer":{"listenAddress":"127.0.0.1","port":9100}}]})",
            "test");
        require(value.maxPendingClients == boundary && value.rawServerMinThreads == boundary &&
            value.lpdMinThreads == boundary && value.lpdServer.port == boundary, "Integer boundary changed");
        const auto raw = ParseConfigurationJson(R"({"printerMap":[{"name":"Printer","lprQueue":"Queue","rawServer":{"port":)" +
            token + "}}]}", "test");
        require(raw.printerMap[0].rawServer->port == boundary, "RAW port boundary changed");
    }
}

void testSemanticErrors() {
    const std::vector<std::pair<std::string, std::string>> invalid{
        {R"({"printerMap":[{"name":"Printer","lprQueue":"Queue"},{"name":"Other","lprQueue":"qUEUE"}]})",
            "printerMap[1].lprQueue"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":"A","rawServer":{}},{"name":"Other","lprQueue":"B","rawServer":{}}]})",
            "printerMap[1].rawServer"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":"A","rawServer":{"listenAddress":"001.002.003.004"}},
            {"name":"Other","lprQueue":"B","rawServer":{"listenAddress":"1.2.3.4"}}]})", "printerMap[1].rawServer"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":"Queue","rawServer":{"port":515}}]})",
            "printerMap[0].rawServer"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":"Queue","rawServer":{"listenAddress":"127.0.0.1","port":515}}]})",
            "printerMap[0].rawServer"},
        {R"({"lpdServer":{"listenAddress":"127.0.0.1"},
            "printerMap":[{"name":"Printer","lprQueue":"Queue","rawServer":{"port":515}}]})",
            "printerMap[0].rawServer"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":"A","rawServer":{}},
            {"name":"Other","lprQueue":"B","rawServer":{"listenAddress":"127.0.0.1"}}]})", "printerMap[1].rawServer"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":"A","rawServer":{"listenAddress":"127.0.0.1"}},
            {"name":"Other","lprQueue":"B","rawServer":{}}]})", "printerMap[1].rawServer"},
        {R"({"lpdServer":{"listenAddress":"256.0.0.1"}})", "lpdServer.listenAddress"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":"Queue","rawServer":{"listenAddress":"::1"}}]})",
            "printerMap[0].rawServer.listenAddress"},
        {R"({"logLevel":"0"})", "logLevel"},
        {R"({"logLevel":"6"})", "logLevel"},
        {R"({"logLevel":"warning"})", "logLevel"},
        {R"({"logLevel":"verbose"})", "logLevel"},
    };
    for (const auto& [json, field] : invalid) {
        runTest(field, [&] { requireInvalid(json, field); });
    }
    auto separateDocument = Json::parse(R"({
        "ppmDisabled":true,
        "lpdServer":{"listenAddress":"127.0.0.1"},
        "printerMap":[
            {"name":"Printer","lprQueue":"A","rawServer":{"listenAddress":"127.0.0.2","port":515}},
            {"name":"Printer","lprQueue":"B","rawServer":{"listenAddress":"127.0.0.3","port":515}}
        ]
    })");
    const auto separate = ParseConfigurationJson(separateDocument.dump(), "test");
    require(separate.ppmDisabled && separate.printerMap.size() == 2 && separate.printerMap[1].rawServer->port == 515,
        "Different specific addresses may share a port when PPM is disabled");
    separateDocument["ppmDisabled"] = false;
    requireInvalid(separateDocument.dump(), "printerMap[0].rawServer.listenAddress", "ppmDisabled=true");
}

void testPpmRules() {
    for (const auto& queue : std::vector<std::string>{"two words", std::string(33, 'q'),
        "Q=R", "Q\tR", std::string("Q\x1f", 2), std::string("Q\x7f", 2)}) {
        auto document = Json::parse(R"({"printerMap":[{"name":"Printer","lprQueue":"Queue"}]})");
        document["printerMap"][0]["lprQueue"] = queue;
        requireInvalid(document.dump(), "printerMap[0].lprQueue", "PPM");
        document["ppmDisabled"] = true;
        document["lpdServer"] = {{"port", 1515}};
        const auto value = ParseConfigurationJson(document.dump(), "test");
        require(value.ppmDisabled && value.lpdServer.port == 1515 &&
            value.printerMap[0].lprQueue == queue, "PPM opt-out must relax queue and port rules");
    }
    for (const auto* queue : {"Q\rR", "Q\nR", "Q\r\nR"}) {
        auto document = Json::parse(R"({"printerMap":[{"name":"Printer","lprQueue":"Queue"}]})");
        document["printerMap"][0]["lprQueue"] = queue;
        for (const auto disabled : {false, true}) {
            document["ppmDisabled"] = disabled;
            requireInvalid(document.dump(), "printerMap[0].lprQueue", "CR or LF");
        }
    }
    requireInvalid(R"({"lpdServer":{"port":1515},"printerMap":[{"name":"Printer","lprQueue":"Queue"}]})",
        "lpdServer.port", "ppmDisabled=true");
}

void testEncoding() {
    require(Utf8ToWide("Printer") == L"Printer" && WideToUtf8(L"Printer") == "Printer", "UTF-8 ASCII conversion");
    require(WideToAcp(L"Printer") == "Printer" && Utf8ToAcp("Printer") == "Printer" &&
        AcpToUtf8("Printer") == "Printer", "ACP ASCII conversion");
    require(Utf8ToWide("").empty() && WideToUtf8(L"").empty() &&
        WideToAcp(L"").empty() && Utf8ToAcp("").empty() && AcpToUtf8("").empty(), "Empty encoding conversions");
    for (const auto& invalid : {bytes({0xc0, 0xaf}), bytes({0xe2, 0x82}), bytes({0xed, 0xa0, 0x80}),
        bytes({0xf4, 0x90, 0x80, 0x80})}) {
        requireThrows([&] { Utf8ToWide(invalid); }, "Malformed UTF-8 accepted");
        requireThrows([&] { Utf8ToAcp(invalid); }, "Malformed UTF-8 accepted by ACP conversion");
    }
    requireThrows([] { WideToUtf8(std::wstring(1, static_cast<wchar_t>(0xd800))); },
        "Unpaired UTF-16 surrogate accepted");
    if (GetACP() == CP_UTF8) {
        requireThrows([] { AcpToUtf8(bytes({0xc0, 0xaf})); }, "Malformed UTF-8 ACP text accepted");
    } else {
        for (unsigned int value = 1; value <= 255; ++value) {
            if (IsDBCSLeadByteEx(CP_ACP, static_cast<BYTE>(value))) {
                requireThrows([&] { AcpToUtf8(std::string(1, static_cast<char>(value))); },
                    "Incomplete ACP multibyte sequence accepted");
                break;
            }
        }
    }

    constexpr auto emoji = R"({"printerMap":[{"name":"\uD83D\uDE00","lprQueue":"Queue"}]})";
    if (GetACP() == CP_UTF8) {
        // Unicode is representable when Windows explicitly uses UTF-8 as its ACP.
        const auto value = ParseConfigurationJson(emoji, "test");
        require(value.printerMap[0].name == bytes({0xf0, 0x9f, 0x98, 0x80}), "UTF-8 ACP name");
        requireSame(ParseConfigurationJson(SerializeConfigurationJson(value), "test"), value);
    } else {
        requireInvalid(emoji, "printerMap[0].name", "cannot be encoded");
    }
    const std::vector<std::pair<std::string, std::string>> nulStrings{
        {R"({"logLevel":"info\u0000"})", "logLevel"},
        {R"({"lpdServer":{"listenAddress":"127.0.0.1\u0000"}})", "lpdServer.listenAddress"},
        {R"({"printerMap":[{"name":"P\u0000rinter","lprQueue":"Queue"}]})", "printerMap[0].name"},
        {R"({"ppmDisabled":true,"printerMap":[{"name":"Printer","lprQueue":"Q\u0000ueue"}]})", "printerMap[0].lprQueue"},
        {R"({"printerMap":[{"name":"Printer","lprQueue":"Queue","rawServer":{"listenAddress":"0.0.0.0\u0000"}}]})",
            "printerMap[0].rawServer.listenAddress"},
    };
    for (const auto& [json, field] : nulStrings) requireInvalid(json, field, "NUL");
    requireInvalid(std::string(R"({"printerMap":[{"name":")") + bytes({0xff}) + R"(","lprQueue":"Queue"}]})",
        "invalid JSON");
}

void testSerialization() {
    for (const auto& value : std::vector<ServiceConfiguration>{ServiceConfiguration{},
        ParseConfigurationJson(kFullConfiguration, "test"),
        ParseConfigurationJson(R"({"printerMap":[{"name":"Printer","lprQueue":"Queue","rawServer":{}}]})", "test")}) {
        const auto serialized = SerializeConfigurationJson(value);
        require(!serialized.empty() && serialized.back() == '\n', "Serialization must end with a newline");
        require(serialized.find("\n  \"") != std::string::npos, "Serialization must use two-space indentation");
        requireSame(ParseConfigurationJson(serialized, "roundtrip"), value);
        const auto document = Json::parse(serialized);
        require(document.size() == 6 && document.contains("logLevel") && document.contains("maxPendingClients") &&
            document.contains("lpdServer") && document.contains("minThreads") && document.contains("printerMap") &&
            document.contains("ppmDisabled"), "All schema fields must be explicit");
        require(document["lpdServer"].size() == 2 && document["lpdServer"].contains("listenAddress") &&
            document["lpdServer"].contains("port"), "LPD defaults must be explicit");
        require(document["minThreads"].size() == 2 && document["minThreads"].contains("raw") &&
            document["minThreads"].contains("lpd"), "Thread defaults must be explicit");
        for (size_t index = 0; index < value.printerMap.size(); ++index) {
            const auto& mapping = document["printerMap"][index];
            if (value.printerMap[index].rawServer) {
                require(mapping.size() == 3 && mapping["rawServer"].size() == 2 &&
                    mapping["rawServer"].contains("listenAddress") && mapping["rawServer"].contains("port"),
                    "RAW defaults must be explicit");
            } else {
                require(mapping.size() == 2 && !mapping.contains("rawServer"), "LPD-only mapping must omit rawServer");
            }
        }
    }
    ServiceConfiguration invalid;
    invalid.maxPendingClients = 0;
    requireThrows([&] { SerializeConfigurationJson(invalid); }, "Serialization skipped validation");
    invalid = ServiceConfiguration{};
    invalid.printerMap = {{std::string("P\0Q", 3), "Queue", std::nullopt}};
    requireThrows([&] { SerializeConfigurationJson(invalid); }, "Serialization accepted embedded NUL");
}

void testLoadFiles() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "config.json";
    writeFile(path, "");
    requireThrows([&] { LoadConfigurationFile(path); }, "Empty file accepted");
    writeFile(path, "[");
    requireThrows([&] { LoadConfigurationFile(path); }, "Malformed file accepted");
    writeFile(path, std::string(kMaxConfigurationFileBytes + 1, ' '));
    requireThrows<std::runtime_error>([&] { LoadConfigurationFile(path); }, "Oversized file accepted");
    writeFile(path, std::string("{}") + std::string(kMaxConfigurationFileBytes - 2, ' '));
    requireSame(LoadConfigurationFile(path), ServiceConfiguration{});
    writeFile(path, bytes({0xef, 0xbb, 0xbf}) + "{}");
    requireSame(LoadConfigurationFile(path), ServiceConfiguration{});
    writeFile(path, kFullConfiguration);
    requireSame(LoadConfigurationFile(path), ParseConfigurationJson(kFullConfiguration, "test"));

    const auto missing = directory.path() / L"missing-\u6253\u5370.json";
    try {
        LoadConfigurationFile(missing);
        throw std::runtime_error("Missing file accepted");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        require(message.find("Win32 error") != std::string::npos, "I/O error must include a Win32 code");
        require(message.find(WideToUtf8(missing.native())) != std::string::npos, "I/O error must include the UTF-8 path");
    }
}

void testSaveFiles() {
    TemporaryDirectory directory;
    const auto path = directory.path() / L"config-\u6253\u5370.json";
    const auto initial = ParseConfigurationJson(kFullConfiguration, "test");
    SaveConfigurationFile(path, initial);
    requireSame(LoadConfigurationFile(path), initial);
    require(!std::filesystem::exists(temporarySavePath(path)), "Successful save left a temporary file");
    SaveConfigurationFile(path, ServiceConfiguration{});
    requireSame(LoadConfigurationFile(path), ServiceConfiguration{});
    auto replacement = initial;
    replacement.printerMap[0].name = "Replacement Printer";
    SaveConfigurationFile(path, replacement);
    requireSame(LoadConfigurationFile(path), replacement);

    auto invalid = replacement;
    invalid.printerMap[0].name.clear();
    requireThrows([&] { SaveConfigurationFile(path, invalid); }, "Invalid configuration saved");
    requireSame(LoadConfigurationFile(path), replacement);
    require(!std::filesystem::exists(temporarySavePath(path)), "Invalid save left a temporary file");

    auto oversized = ServiceConfiguration{};
    oversized.printerMap = {{std::string(kMaxConfigurationFileBytes, 'p'), "Queue", std::nullopt}};
    requireThrows<std::runtime_error>([&] { SaveConfigurationFile(path, oversized); }, "Oversized configuration saved");
    requireSame(LoadConfigurationFile(path), replacement);

    const auto missingParent = directory.path() / "missing" / "config.json";
    requireThrows<std::runtime_error>([&] { SaveConfigurationFile(missingParent, initial); }, "Missing parent accepted");
    require(!std::filesystem::exists(temporarySavePath(missingParent)), "Failed create left a temporary file");

    const auto targetDirectory = directory.path() / "directory.json";
    require(std::filesystem::create_directory(targetDirectory), "Cannot create failure target");
    requireThrows<std::runtime_error>([&] { SaveConfigurationFile(targetDirectory, initial); }, "Directory replacement accepted");
    require(std::filesystem::is_directory(targetDirectory), "Failed replacement changed the target");
    require(!std::filesystem::exists(temporarySavePath(targetDirectory)), "Failed rename left a temporary file");

    const auto collision = temporarySavePath(path);
    writeFile(collision, "{}");
    requireThrows<std::runtime_error>([&] { SaveConfigurationFile(path, initial); }, "CREATE_NEW temporary collision accepted");
    requireSame(LoadConfigurationFile(path), replacement);
    requireSame(LoadConfigurationFile(collision), ServiceConfiguration{});
}

} // namespace

int main() {
    try {
        runTest("defaults and minimal mapping", testDefaultsAndMinimalMapping);
        runTest("full configuration", testFullConfiguration);
        runTest("schema errors", testSchemaErrors);
        runTest("type and range errors", testTypeAndRangeErrors);
        runTest("semantic errors", testSemanticErrors);
        runTest("PPM rules", testPpmRules);
        runTest("encoding", testEncoding);
        runTest("serialization", testSerialization);
        runTest("load files", testLoadFiles);
        runTest("save files", testSaveFiles);
    } catch (const std::exception& error) {
        std::cerr << "Configuration file tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
