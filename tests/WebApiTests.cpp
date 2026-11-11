#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <Windows.h>

#include <WebServer.h>
#include <ConfigurationFile.hpp>

#include "TestSupport.h"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <system_error>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace {

using namespace printhub::web;
using Json = nlohmann::json;
constexpr std::string_view kToken = "0123456789abcdef";
constexpr auto kStatusJson = R"({"version":"test","mode":"console","uptimeSeconds":12})";
constexpr auto kConfigJson = R"({"ppmDisabled":true,"printerMap":[]})";
constexpr auto kPrintersJson = R"({"printers":["Microsoft Print to PDF"]})";

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::atomic_uint sequence{0};
        const auto root = std::filesystem::current_path();
        for (int attempt = 0; attempt < 64; ++attempt) {
            auto candidate = root / ("PrintHubWebApiTest-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1)));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
            if (error) throw std::runtime_error("Cannot create web test directory: " + error.message());
        }
        throw std::runtime_error("Cannot choose a unique web test directory");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.is_open(), "Cannot open test file");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    require(!output.fail(), "Cannot write test file");
}

std::string numberedLines(int count) {
    std::string contents;
    for (int line = 1; line <= count; ++line) contents += "line " + std::to_string(line) + "\r\n";
    return contents;
}

httplib::Headers tokenHeaders() {
    return {{"X-PrintHub-Token", std::string(kToken)}};
}

httplib::Result authenticatedGet(httplib::Client& client, const std::string& path) {
    return client.Get(path, tokenHeaders());
}

httplib::Result authenticatedPut(httplib::Client& client, const std::string& body,
    const std::string& contentType = "application/json") {
    return client.Put("/api/config", tokenHeaders(), body, contentType);
}

httplib::Response expectResponse(httplib::Result result, int status, bool api = true) {
    require(static_cast<bool>(result), "HTTP request failed, error " +
        std::to_string(static_cast<int>(result.error())));
    require(result->status == status, "Unexpected HTTP status: " + std::to_string(result->status));
    require(result->get_header_value("X-Content-Type-Options") == "nosniff", "Missing nosniff header");
    require(!result->has_header("Access-Control-Allow-Origin"), "API must not enable CORS");
    if (api) {
        require(result->get_header_value("Cache-Control") == "no-store", "Missing API no-store header");
        require(result->get_header_value("Content-Type").find("application/json") != std::string::npos,
            "Missing JSON content type");
    }
    return *result;
}

void expectError(httplib::Result result, int status, std::string_view message) {
    const auto response = expectResponse(std::move(result), status);
    require(Json::parse(response.body) == Json{{"error", std::string(message)}}, "Unexpected JSON error");
}

class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Cannot initialize test Winsock");
    }
    ~SocketRuntime() { WSACleanup(); }
    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};

class TestSocket {
public:
    TestSocket() : socket_(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) {
        require(socket_ != INVALID_SOCKET, "Cannot create test socket");
    }
    ~TestSocket() { close(); }
    TestSocket(const TestSocket&) = delete;
    TestSocket& operator=(const TestSocket&) = delete;
    SOCKET get() const { return socket_; }
    void close() {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }
private:
    SOCKET socket_;
};

void waitForSocket(SOCKET socket, bool writing, std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    require(remaining > 0, "Raw HTTP request exceeded its 2s deadline (server may be waiting for body)");
    timeval timeout{static_cast<long>(remaining / 1000000), static_cast<long>(remaining % 1000000)};
    fd_set ready, errors;
    FD_ZERO(&ready);
    FD_ZERO(&errors);
    FD_SET(socket, &ready);
    FD_SET(socket, &errors);
    require(select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, &errors, &timeout) > 0,
        "Raw HTTP socket timed out or failed");
}

int rawRequestStatus(std::uint16_t port, std::string_view headers) {
    SocketRuntime winsock;
    TestSocket socket;
    const DWORD timeoutMs = 2000;
    for (const auto option : {SO_RCVTIMEO, SO_SNDTIMEO}) {
        require(setsockopt(socket.get(), SOL_SOCKET, option, reinterpret_cast<const char*>(&timeoutMs),
            sizeof(timeoutMs)) == 0, "Cannot set test socket timeout");
    }
    u_long nonblocking = 1;
    require(ioctlsocket(socket.get(), FIONBIO, &nonblocking) == 0, "Cannot make test socket nonblocking");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        require(WSAGetLastError() == WSAEWOULDBLOCK, "Cannot connect raw HTTP socket");
        waitForSocket(socket.get(), true, deadline);
        int error = 0;
        int size = sizeof(error);
        require(getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0 &&
            error == 0, "Raw HTTP connection failed");
    }
    for (std::size_t sent = 0; sent < headers.size();) {
        waitForSocket(socket.get(), true, deadline);
        const auto count = send(socket.get(), headers.data() + sent, static_cast<int>(headers.size() - sent), 0);
        if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        require(count > 0, "Cannot send raw HTTP headers");
        sent += static_cast<std::size_t>(count);
    }
    // Do not send a body or half-close: either would mask a late rejection.
    // One absolute deadline also bounds trickled response bytes and connect().
    std::string response;
    std::array<char, 256> buffer{};
    while (response.find("\r\n") == std::string::npos) {
        require(response.size() < 1024, "Raw HTTP status line is too long");
        waitForSocket(socket.get(), false, deadline);
        const auto count = recv(socket.get(), buffer.data(), static_cast<int>(buffer.size()), 0);
        if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        require(count > 0, "Connection closed without a raw HTTP status line");
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    require(response.starts_with("HTTP/1.1 ") && response.size() >= 13, "Invalid raw HTTP status line");
    int status = 0;
    const auto [end, error] = std::from_chars(response.data() + 9, response.data() + 12, status);
    require(error == std::errc{} && end == response.data() + 12, "Invalid raw HTTP status code");
    return status;
}

class ServerFixture {
public:
    explicit ServerFixture(const std::function<void(Options&)>& customize = {}) {
        Options options;
        options.port = 0;
        options.accessToken = kToken;
        options.logDirectory = directory.path() / "logs";
        options.version = "test";
        options.statusJson = [] { return std::string(kStatusJson); };
        options.configJson = [] { return std::string(kConfigJson); };
        options.printersJson = [] { return std::string(kPrintersJson); };
        options.applyConfig = [](std::string_view body) {
            return ApplyResponse{200, Json{{"received", std::string(body)}}.dump()};
        };
        require(std::filesystem::create_directory(options.logDirectory), "Cannot create log directory");
        if (customize) customize(options);
        server = std::make_unique<Server>(std::move(options));
        port = server->Start();
        require(port != 0 && server->IsRunning(), "Server did not start");
        client = std::make_unique<httplib::Client>("127.0.0.1", port);
        client->set_connection_timeout(2, 0);
        client->set_read_timeout(3, 0);
        client->set_write_timeout(3, 0);
        client->set_keep_alive(false);
    }
    ~ServerFixture() {
        if (server) server->Stop();
    }
    std::filesystem::path logDirectory() const { return directory.path() / "logs"; }

    TemporaryDirectory directory;
    std::uint16_t port = 0;
    std::unique_ptr<Server> server;
    std::unique_ptr<httplib::Client> client;
};

void testTokenValidation() {
    require(IsValidAccessToken(kToken), "16-byte token rejected");
    require(IsValidAccessToken(std::string(256, '!')), "256-byte token rejected");
    require(IsValidAccessToken(std::string(16, '~')), "Printable ASCII upper boundary rejected");
    for (const auto size : {0, 1, 15, 257}) {
        require(!IsValidAccessToken(std::string(size, 'a')), "Invalid token length accepted");
    }
    for (const unsigned char character : {0x00, 0x09, 0x0a, 0x0d, 0x20, 0x7f, 0x80, 0xff}) {
        auto token = std::string(kToken);
        token[8] = static_cast<char>(character);
        require(!IsValidAccessToken(token), "Non-printable or space token byte accepted");
    }
}

void testInvalidTokenStart() {
    for (const auto& token : std::vector<std::string>{"", std::string(15, 'x'), "01234567 89abcdef",
        std::string(257, 'x'), "0123456789abcdef\x80"}) {
        Options options;
        options.port = 0;
        options.accessToken = token;
        Server server(options);
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool rejected = false;
            try {
                (void)server.Start();
            } catch (const std::runtime_error& error) {
                rejected = true;
                require(std::string(error.what()) ==
                    "Invalid web access token: expected 16..256 printable ASCII characters",
                    "Invalid token startup diagnostic changed");
            }
            require(rejected, "Invalid token accepted by Start");
            require(!server.IsRunning(), "Invalid token left server running");
            server.Stop();
        }
    }
}

void testLifecycle() {
    Options options;
    options.port = 0;
    options.accessToken = kToken;
    Server server(options);
    require(!server.IsRunning(), "Unstarted server reports running");
    server.Stop();
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto port = server.Start();
        require(port != 0 && server.IsRunning(), "Start did not bind an ephemeral port");
        require(server.Start() == port, "Repeated Start changed the port");
        server.Stop();
        require(!server.IsRunning(), "Stop left server running");
        server.Stop();
    }
    options.listenAddress = "localhost";
    Server invalid(options);
    requireThrows<std::runtime_error>([&] { (void)invalid.Start(); }, "Non-IPv4 listen address accepted");
    options.listenAddress = "127.000.000.001";
    Server decimal(options);
    require(decimal.Start() != 0 && decimal.IsRunning(), "Decimal leading-zero IPv4 rejected");
    decimal.Stop();
}

void testBindFailureRetry() {
    SocketRuntime winsock;
    TestSocket occupied;
    const BOOL exclusive = TRUE;
    require(setsockopt(occupied.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0, "Cannot reserve exclusive port");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(occupied.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
        "Cannot bind occupied test port");
    require(listen(occupied.get(), SOMAXCONN) == 0, "Cannot listen on occupied test port");
    int size = sizeof(address);
    require(getsockname(occupied.get(), reinterpret_cast<sockaddr*>(&address), &size) == 0,
        "Cannot discover occupied test port");
    Options options;
    options.port = ntohs(address.sin_port);
    options.accessToken = kToken;
    Server server(options);
    requireThrows<std::runtime_error>([&] { (void)server.Start(); }, "Start accepted an occupied port");
    require(!server.IsRunning(), "Bind failure left server running");
    occupied.close();
    require(server.Start() == options.port && server.IsRunning(), "Same server could not retry a failed bind");
    server.Stop();
}

void testEarlyRejection() {
    std::atomic_uint calls{0};
    ServerFixture fixture([&](Options& options) {
        options.applyConfig = [&](std::string_view) {
            calls.fetch_add(1);
            return ApplyResponse{200, "{}"};
        };
    });
    const auto request = [&](std::string_view method, std::string_view path, std::string_view headers,
        int expected, bool authenticated = true) {
        std::string wire = std::string(method) + " " + std::string(path) +
            " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
        if (authenticated) wire += "X-PrintHub-Token: " + std::string(kToken) + "\r\n";
        wire += headers;
        wire += "\r\n";
        const auto status = rawRequestStatus(fixture.port, wire);
        require(status == expected, "Wrong early status for " + std::string(method) + " " +
            std::string(path) + ": " + std::to_string(status));
    };
    request("POST", "/", "Content-Length: 1000000\r\n", 405, false);
    request("PUT", "/api/config", "Transfer-Encoding: chunked\r\n", 411);
    request("PUT", "/api/config", "", 411);
    request("PUT", "/api/config", "Content-Length: " + std::to_string(kMaxConfigurationFileBytes + 1) + "\r\n", 413);
    request("GET", "/api/status", "Content-Length: 1\r\n", 400);
    request("HEAD", "/", "Content-Length: 1\r\n", 400, false);
    request("GET", "/api/status", "Transfer-Encoding: chunked\r\n", 400);
    request("PUT", "/api/config", "Content-Length: invalid\r\n", 411);
    request("PUT", "/api/config", "Content-Length: -1\r\n", 411);
    request("PUT", "/api/config", "Content-Length: 18446744073709551616\r\n", 411);
    request("PUT", "/api/config", "Content-Length: 1\r\nContent-Length: 1\r\n", 411);
    request("PUT", "/api/config", "Transfer-Encoding: chunked\r\nContent-Length: 1\r\n", 411);
    request("PUT", "/api/config", "Transfer-Encoding: chunked\r\n", 401, false);
    request("PUT", "/api/config", "Content-Length: 1000000\r\n", 401, false);
    for (const auto* method : {"POST", "PATCH", "DELETE"}) {
        request(method, "/api/config", "Content-Length: 1000000\r\n", 405);
    }
    request("PUT", "/nope", "Content-Length: 1000000\r\n", 405, false);
    request("OPTIONS", "/api/config", "Content-Length: 1\r\n", 405);
    require(calls.load() == 0, "An early rejection reached the apply callback");
}

void testReadLogLines() {
    TemporaryDirectory directory;
    require(Json::parse(ReadRecentLogLinesJson(directory.path(), 5)) == Json{{"lines", Json::array()}},
        "Empty log directory must return an empty array");
    require(Json::parse(ReadRecentLogLinesJson(directory.path() / "missing", 5))["lines"].empty(),
        "Missing log directory must return an empty array");
    writeFile(directory.path() / "PrintHub_2026-01-01.log", "old\n");
    writeFile(directory.path() / "Other_2099-01-01.log", "ignore\n");
    writeFile(directory.path() / "PrintHub_2099-01-01.txt", "ignore\n");
    require(std::filesystem::create_directory(directory.path() / "PrintHub_2099-01-02.log"),
        "Cannot create non-file log entry");
    const auto latest = directory.path() / "PrintHub_2026-01-02.log";
    writeFile(latest, numberedLines(6) + "line 7");
    require(Json::parse(ReadRecentLogLinesJson(directory.path(), 5))["lines"] ==
        Json::array({"line 3", "line 4", "line 5", "line 6", "line 7"}), "Wrong latest log tail");
    require(Json::parse(ReadRecentLogLinesJson(directory.path(), 0))["lines"] == Json::array({"line 7"}),
        "Log tail lower bound must be one");
    writeFile(latest, numberedLines(550));
    const auto capped = Json::parse(ReadRecentLogLinesJson(directory.path(), 9999))["lines"];
    require(capped.size() == 500 && capped.front() == "line 51" && capped.back() == "line 550",
        "Log tail upper bound must be 500 without a phantom trailing line");
    writeFile(latest, "first\r\n\r\nlast\r\n");
    require(Json::parse(ReadRecentLogLinesJson(directory.path(), 10))["lines"] ==
        Json::array({"first", "", "last"}), "CRLF or empty-line handling changed");
    writeFile(latest, "bad " + bytes({0xff}) + "\n");
    require(Json::parse(ReadRecentLogLinesJson(directory.path(), 5))["lines"] ==
        Json::array({"bad " + bytes({0xef, 0xbf, 0xbd})}), "Malformed UTF-8 was not replaced");
    writeFile(latest, std::string(70 * 1024, 'x') + "\r\nkept 1\r\nkept 2\n");
    require(Json::parse(ReadRecentLogLinesJson(directory.path(), 5))["lines"] ==
        Json::array({"kept 1", "kept 2"}), "Partial first line from the read window was not dropped");
    writeFile(latest, std::string(70 * 1024, 'x'));
    require(Json::parse(ReadRecentLogLinesJson(directory.path(), 5))["lines"].empty(),
        "A truncated single line must not be returned");
    const auto completeLine = std::string(64 * 1024 - 5, 'x');
    writeFile(latest, "prefix\n" + completeLine + "\nlast");
    require(Json::parse(ReadRecentLogLinesJson(directory.path(), 5))["lines"] ==
        Json::array({completeLine, "last"}), "Complete line at the 64 KiB boundary was lost");
    writeFile(latest, "");
    require(Json::parse(ReadRecentLogLinesJson(directory.path(), 5))["lines"].empty(),
        "Empty latest log must not fall back to an older file");
}

void testGetRoutesAndAuthentication() {
    ServerFixture fixture;
    auto& client = *fixture.client;
    for (const auto* path : {"/", "/index.html"}) {
        const auto response = expectResponse(client.Get(path), 200, false);
        require(response.body.find("<title>PrintHub") != std::string::npos, "Embedded HTML is missing");
        require(response.get_header_value("Content-Type") == "text/html; charset=utf-8", "Wrong HTML content type");
        require(response.get_header_value("Cache-Control") == "no-store", "HTML must not be cached");
        require(response.get_header_value("X-Frame-Options") == "DENY", "HTML must deny framing");
        require(response.get_header_value("Content-Security-Policy").find("frame-ancestors 'none'") !=
            std::string::npos, "HTML response is missing its frame-ancestors policy");
    }
    for (const auto* path : {"/api/status", "/api/config", "/api/logs", "/api/unknown"}) {
        expectError(client.Get(path), 401, "unauthorized");
    }
    require(expectResponse(authenticatedGet(client, "/api/status"), 200).body == kStatusJson,
        "Status callback body changed");
    require(expectResponse(authenticatedGet(client, "/api/config"), 200).body == kConfigJson,
        "Config callback body changed");
    for (const auto& wrong : std::vector<std::string>{"0123456789abcdeg", "1123456789abcdef",
        "0123456789abcde", "0123456789abcdefx", std::string(257, 'x')}) {
        expectError(client.Get("/api/status", {{"X-PrintHub-Token", wrong}}), 401, "unauthorized");
    }
    auto duplicates = tokenHeaders();
    duplicates.emplace("X-PrintHub-Token", std::string(kToken));
    expectError(client.Get("/api/status", duplicates), 401, "unauthorized");
    expectError(authenticatedGet(client, "/api/unknown"), 404, "not found");
    expectError(client.Post("/api/unknown", tokenHeaders(), "{}", "application/json"), 405, "method not allowed");
    expectError(client.Options("/api/config"), 401, "unauthorized");
    expectError(client.Options("/api/config", tokenHeaders()), 404, "not found");
    expectResponse(client.Get("/nope"), 404, false);
    expectResponse(client.Get("/indexXhtml"), 404, false);
}

void testPrintersRoute() {
    ServerFixture fixture;
    auto& client = *fixture.client;
    expectError(client.Get("/api/printers"), 401, "unauthorized");
    expectError(client.Get("/api/printers", {{"X-PrintHub-Token", "0123456789abcdeg"}}),
        401, "unauthorized");
    const auto response = expectResponse(authenticatedGet(client, "/api/printers"), 200);
    require(Json::parse(response.body) == Json::parse(kPrintersJson), "Printers callback body changed");

    ServerFixture failing([](Options& options) {
        options.printersJson = []() -> std::string { throw std::runtime_error("enumeration failed"); };
    });
    expectError(failing.client->Get("/api/printers"), 401, "unauthorized");
    expectError(authenticatedGet(*failing.client, "/api/printers"), 500, "enumeration failed");
}

void testLogRoute() {
    ServerFixture fixture;
    auto& client = *fixture.client;
    require(Json::parse(expectResponse(authenticatedGet(client, "/api/logs"), 200).body)["lines"].empty(),
        "Logs endpoint must handle an empty directory");
    writeFile(fixture.logDirectory() / "PrintHub_2026-01-02.log", numberedLines(550));
    const auto lastFive = Json::parse(expectResponse(authenticatedGet(client, "/api/logs?tail=5"), 200).body);
    require(lastFive["lines"] == Json::array({"line 546", "line 547", "line 548", "line 549", "line 550"}),
        "Logs endpoint did not return the last five lines");
    for (const auto* path : {"/api/logs", "/api/logs?tail=invalid", "/api/logs?tail=2junk",
        "/api/logs?tail=", "/api/logs?tail=2147483648"}) {
        const auto lines = Json::parse(expectResponse(authenticatedGet(client, path), 200).body)["lines"];
        require(lines.size() == 100 && lines.front() == "line 451", "Invalid tail must default to 100");
    }
    for (const auto* path : {"/api/logs?tail=0", "/api/logs?tail=-10"}) {
        require(Json::parse(expectResponse(authenticatedGet(client, path), 200).body)["lines"] ==
            Json::array({"line 550"}), "Tail lower bound must be one");
    }
    const auto capped = Json::parse(expectResponse(authenticatedGet(client, "/api/logs?tail=5000"), 200).body);
    require(capped["lines"].size() == 500 && capped["lines"].front() == "line 51", "Tail upper bound must be 500");
}

void testPutRoute() {
    std::atomic_uint calls{0};
    ServerFixture fixture([&](Options& options) {
        options.applyConfig = [&](std::string_view body) -> ApplyResponse {
            calls.fetch_add(1);
            if (body == R"({"action":"reject"})") return {400, R"({"error":"rejected","field":"name"})"};
            if (body == R"({"action":"conflict"})") return {409, R"({"error":"busy"})"};
            if (body == R"({"action":"failure"})") return {500, R"({"error":"apply failed"})"};
            if (body == R"({"action":"invalid"})") throw std::invalid_argument("invalid \"name\"\nvalue");
            if (body == R"({"action":"throw"})") throw std::runtime_error("apply \"failed\"\nretry");
            if (body == R"({"action":"unknown"})") throw 42;
            return {200, Json{{"received", std::string(body)}}.dump()};
        };
    });
    auto& client = *fixture.client;
    expectError(client.Put("/api/config", "{}", "application/json"), 401, "unauthorized");
    expectError(authenticatedPut(client, "{}", "text/plain"), 415, "unsupported media type");
    expectError(authenticatedPut(client, "{}", "application/jsonp"), 415, "unsupported media type");
    require(calls.load() == 0, "Authentication or media-type rejection invoked apply callback");
    const std::string body = " {\n  \"ppmDisabled\": true\n}\n";
    const auto response = expectResponse(authenticatedPut(client, body, "application/json; charset=utf-8"), 200);
    require(response.body == Json{{"received", body}}.dump(), "Apply did not receive the original request body");
    require(expectResponse(authenticatedPut(client, "{}", "Application/JSON"), 200).body ==
        Json{{"received", "{}"}}.dump(), "Media type must be case insensitive");
    require(expectResponse(authenticatedPut(client, R"({"action":"reject"})"), 400).body ==
        R"({"error":"rejected","field":"name"})", "400 callback response was rewritten");
    expectError(authenticatedPut(client, R"({"action":"conflict"})"), 409, "busy");
    expectError(authenticatedPut(client, R"({"action":"failure"})"), 500, "apply failed");
    expectError(authenticatedPut(client, R"({"action":"invalid"})"), 400, "invalid \"name\"\nvalue");
    expectError(authenticatedPut(client, R"({"action":"throw"})"), 500, "apply \"failed\"\nretry");
    expectError(authenticatedPut(client, R"({"action":"unknown"})"), 500, "internal error");
}

void testCallbackExceptions() {
    ServerFixture fixture([](Options& options) {
        options.statusJson = []() -> std::string { throw std::runtime_error("status \"failed\"\nretry"); };
        options.configJson = []() -> std::string { throw std::invalid_argument("config failure"); };
    });
    expectError(authenticatedGet(*fixture.client, "/api/status"), 500, "status \"failed\"\nretry");
    expectError(authenticatedGet(*fixture.client, "/api/config"), 500, "config failure");
}

void testPayloadLimit() {
    std::atomic_uint calls{0};
    ServerFixture fixture([&](Options& options) {
        options.applyConfig = [&](std::string_view) {
            calls.fetch_add(1);
            return ApplyResponse{200, R"({"applied":true})"};
        };
    });
    std::string body(kMaxConfigurationFileBytes, ' ');
    body.replace(0, 2, "{}");
    expectResponse(authenticatedPut(*fixture.client, body), 200);
    require(calls.load() == 1, "Payload exactly at the limit must reach apply callback");
    // The server now rejects on headers alone. Do not race a client uploading
    // an oversized body against the server closing the rejected connection.
    const auto oversized = "PUT /api/config HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
        "X-PrintHub-Token: " + std::string(kToken) + "\r\nContent-Length: " +
        std::to_string(kMaxConfigurationFileBytes + 1) + "\r\n\r\n";
    require(rawRequestStatus(fixture.port, oversized) == 413, "Oversized request was not rejected before its body");
    require(calls.load() == 1, "Oversized request reached apply callback");
}

} // namespace

int main() {
    try {
        runTest("token validation", testTokenValidation);
        runTest("invalid token startup", testInvalidTokenStart);
        runTest("server lifecycle", testLifecycle);
        runTest("bind failure retry", testBindFailureRetry);
        runTest("early request rejection", testEarlyRejection);
        runTest("log tail helper", testReadLogLines);
        runTest("GET routes and authentication", testGetRoutesAndAuthentication);
        runTest("printers endpoint", testPrintersRoute);
        runTest("log endpoint", testLogRoute);
        runTest("PUT route", testPutRoute);
        runTest("callback exceptions", testCallbackExceptions);
        runTest("payload size limit", testPayloadLimit);
    } catch (const std::exception& error) {
        std::cerr << "Web API tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
