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
#include <Ipv4.h>
#include <TextEncoding.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

extern const unsigned char kWebAssetsIndexHtmlData[];
extern const std::size_t kWebAssetsIndexHtmlSize;

namespace printhub::web {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaxTokenBytes = 256;
constexpr std::size_t kLogReadBytes = 64 * 1024;

class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : handle_(handle) {}
    ~FileHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

[[noreturn]] void fileError(std::string_view operation, const std::filesystem::path& path, DWORD code) {
    throw std::runtime_error(std::string(operation) + " '" + WideToUtf8(path.native()) +
        "': Win32 error " + std::to_string(code));
}

std::string dumpJson(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

void setJson(httplib::Response& response, int status, const std::string& body) {
    response.status = status;
    response.set_content(body, "application/json; charset=utf-8");
}

void setError(httplib::Response& response, int status, std::string_view message) {
    setJson(response, status, dumpJson(Json{{"error", std::string(message)}}));
}

void setResponseHeaders(const httplib::Request& request, httplib::Response& response) {
    if (!response.has_header("X-Content-Type-Options")) {
        response.set_header("X-Content-Type-Options", "nosniff");
    }
    if (!response.has_header("X-Frame-Options")) {
        response.set_header("X-Frame-Options", "DENY");
    }
    if (request.path.starts_with("/api/") && !response.has_header("Cache-Control")) {
        response.set_header("Cache-Control", "no-store");
    }
}

bool equalTokens(std::string_view supplied, std::string_view expected) {
    std::array<unsigned char, kMaxTokenBytes> left{};
    std::array<unsigned char, kMaxTokenBytes> right{};
    std::copy_n(supplied.begin(), std::min(supplied.size(), left.size()), left.begin());
    std::copy_n(expected.begin(), std::min(expected.size(), right.size()), right.begin());
    // Compare the entire padded token, including length, without a mismatch exit.
    // Volatile keeps the comparison from becoming an early-exit library call.
    volatile std::size_t difference = supplied.size() ^ expected.size();
    for (std::size_t index = 0; index < kMaxTokenBytes; ++index) {
        difference = difference | static_cast<std::size_t>(left[index] ^ right[index]);
    }
    return difference == 0;
}

bool isJsonContentType(std::string_view value) {
    value = value.substr(0, value.find(';'));
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) return false;
    value = value.substr(first, value.find_last_not_of(" \t") - first + 1);
    constexpr std::string_view expected = "application/json";
    if (value.size() != expected.size()) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        auto character = value[index];
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
        if (character != expected[index]) return false;
    }
    return true;
}

std::string canonicalIpv4(std::uint32_t address) {
    return std::to_string((address >> 24) & 0xff) + "." +
        std::to_string((address >> 16) & 0xff) + "." +
        std::to_string((address >> 8) & 0xff) + "." + std::to_string(address & 0xff);
}

} // namespace

bool IsValidAccessToken(std::string_view token) {
    return token.size() >= 16 && token.size() <= kMaxTokenBytes &&
        std::all_of(token.begin(), token.end(), [](unsigned char character) {
            return character >= 0x21 && character <= 0x7e;
        });
}

std::string ReadRecentLogLinesJson(const std::filesystem::path& logDirectory, int tailLines) {
    Json result{{"lines", Json::array()}};
    std::filesystem::path latest;
    std::error_code error;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator item(logDirectory, error); !error && item != end;
        item.increment(error)) {
        const auto name = item->path().filename().native();
        std::error_code typeError;
        if (name.starts_with(L"PrintHub_") && name.ends_with(L".log") &&
            item->is_regular_file(typeError) && !typeError &&
            (latest.empty() || name > latest.filename().native())) {
            latest = item->path();
        }
    }
    if (latest.empty()) return dumpJson(result);

    FileHandle file(CreateFileW(latest.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) fileError("Cannot open log file", latest, GetLastError());
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) fileError("Cannot size log file", latest, GetLastError());
    if (size.QuadPart < 0) throw std::runtime_error("Invalid log file size");
    const auto start = std::max<LONGLONG>(0, size.QuadPart - static_cast<LONGLONG>(kLogReadBytes));
    LARGE_INTEGER offset{};
    // Peek at the preceding byte so a complete line at the window boundary is retained.
    offset.QuadPart = start > 0 ? start - 1 : 0;
    if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN)) {
        fileError("Cannot seek log file", latest, GetLastError());
    }
    bool discardPartialLine = false;
    if (start > 0) {
        char preceding = 0;
        DWORD count = 0;
        if (!ReadFile(file.get(), &preceding, 1, &count, nullptr)) {
            fileError("Cannot read log file", latest, GetLastError());
        }
        if (count == 0) return dumpJson(result);
        discardPartialLine = preceding != '\n';
    }
    std::string contents(static_cast<std::size_t>(size.QuadPart - start), '\0');
    std::size_t used = 0;
    while (used < contents.size()) {
        DWORD count = 0;
        if (!ReadFile(file.get(), contents.data() + used,
            static_cast<DWORD>(contents.size() - used), &count, nullptr)) {
            fileError("Cannot read log file", latest, GetLastError());
        }
        if (count == 0) break;
        used += count;
    }
    contents.resize(used);
    std::string_view remaining(contents);
    if (discardPartialLine) {
        const auto newline = remaining.find('\n');
        if (newline == std::string_view::npos) return dumpJson(result);
        remaining.remove_prefix(newline + 1);
    }
    std::vector<std::string_view> lines;
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        auto line = remaining.substr(0, newline);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        if (newline == std::string_view::npos) break;
        remaining.remove_prefix(newline + 1);
    }
    const auto count = std::min(lines.size(), static_cast<std::size_t>(std::clamp(tailLines, 1, 500)));
    for (auto index = lines.size() - count; index < lines.size(); ++index) {
        result["lines"].push_back(std::string(lines[index]));
    }
    return dumpJson(result);
}

struct Server::Impl {
    struct HttpServer : httplib::Server {
        void closeUnstartedSocket() {
            // httplib 0.20 stop() does not close a bound socket until its
            // listener is running. Use only before launch or after an exception
            // has unwound listen_after_bind(), never beside an active listener.
            const auto socket = svr_sock_.exchange(INVALID_SOCKET);
            if (socket != INVALID_SOCKET) closesocket(socket);
        }
    };

    explicit Impl(Options value) : options(std::move(value)) {
        server.new_task_queue = [] { return new httplib::ThreadPool(4, 16); };
        server.set_payload_max_length(kMaxConfigurationFileBytes);
        server.set_read_timeout(10, 0);
        server.set_write_timeout(30, 0);
        server.set_default_headers({{"X-Content-Type-Options", "nosniff"}});
        server.set_pre_routing_handler([this](const httplib::Request& request, httplib::Response& response) {
            setResponseHeaders(request, response);
            if (request.path.starts_with("/api/") &&
                (request.get_header_value_count("X-PrintHub-Token") != 1 ||
                    !equalTokens(request.get_header_value("X-PrintHub-Token"), token))) {
                setError(response, 401, "unauthorized");
                response.set_header("Connection", "close");
                return httplib::Server::HandlerResponse::Handled;
            }
            // routing() invokes this before reading any content. In httplib
            // 0.20, chunked input does not enforce payload_max_length.
            const auto reject = [&](int status, std::string_view message) {
                setError(response, status, message);
                response.set_header("Connection", "close");
                return httplib::Server::HandlerResponse::Handled;
            };
            if ((request.method == "GET" || request.method == "HEAD") &&
                (request.has_header("Content-Length") || request.has_header("Transfer-Encoding"))) {
                return reject(400, "request body not allowed");
            }
            if (request.has_header("Transfer-Encoding")) {
                return reject(411, "content length required; transfer encoding not supported");
            }
            if (request.method == "POST" || request.method == "PUT" ||
                request.method == "PATCH" || request.method == "DELETE") {
                if (request.method != "PUT" || request.path != "/api/config") {
                    return reject(405, "method not allowed");
                }
                if (request.get_header_value_count("Content-Length") != 1) {
                    return reject(411, "valid content length required");
                }
                const auto value = request.get_header_value("Content-Length");
                std::uint64_t length = 0;
                const auto [position, error] = std::from_chars(value.data(), value.data() + value.size(), length);
                if (error != std::errc{} || position != value.data() + value.size()) {
                    return reject(411, "valid content length required");
                }
                if (length > kMaxConfigurationFileBytes) return reject(413, "payload too large");
            } else if (request.has_header("Content-Length")) {
                return reject(405, "method not allowed");
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.set_post_routing_handler(setResponseHeaders);
        server.set_error_handler([](const httplib::Request& request, httplib::Response& response) {
            setResponseHeaders(request, response);
            if (!response.body.empty()) return;
            if (request.path.starts_with("/api/")) {
                if (response.status == 404) setError(response, 404, "not found");
                if (response.status == 413) setError(response, 413, "payload too large");
            } else if (response.status == 404) {
                response.set_content("not found", "text/plain; charset=utf-8");
            }
        });

        const auto html = [](const httplib::Request&, httplib::Response& response) {
            response.set_header("Cache-Control", "no-store");
            response.set_header("Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; "
                "style-src 'unsafe-inline'; img-src data:; connect-src 'self'; frame-ancestors 'none'; "
                "base-uri 'none'; form-action 'none'");
            response.set_content(reinterpret_cast<const char*>(kWebAssetsIndexHtmlData),
                kWebAssetsIndexHtmlSize, "text/html; charset=utf-8");
        };
        server.Get("/", html);
        server.Get(R"(/index\.html)", html);
        server.Get("/api/status", [this](const httplib::Request&, httplib::Response& response) {
            getJson(options.statusJson, response);
        });
        server.Get("/api/config", [this](const httplib::Request&, httplib::Response& response) {
            getJson(options.configJson, response);
        });
        server.Put("/api/config", [this](const httplib::Request& request, httplib::Response& response) {
            if (!isJsonContentType(request.get_header_value("Content-Type"))) {
                setError(response, 415, "unsupported media type");
                return;
            }
            try {
                const auto applied = options.applyConfig(request.body);
                setJson(response, applied.status, applied.body);
            } catch (const std::invalid_argument& error) {
                setError(response, 400, error.what());
            } catch (const std::exception& error) {
                setError(response, 500, error.what());
            } catch (...) {
                setError(response, 500, "internal error");
            }
        });
        server.Get("/api/logs", [this](const httplib::Request& request, httplib::Response& response) {
            int tail = 100;
            if (request.has_param("tail")) {
                const auto value = request.get_param_value("tail");
                int parsed = 0;
                const auto [position, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (error == std::errc{} && position == value.data() + value.size()) tail = parsed;
            }
            getJson([&] { return ReadRecentLogLinesJson(options.logDirectory, std::clamp(tail, 1, 500)); }, response);
        });
        server.Get("/api/printers", [this](const httplib::Request&, httplib::Response& response) {
            getJson(options.printersJson, response);
        });
        server.Get(R"(/api/.*)", [](const httplib::Request&, httplib::Response& response) {
            setError(response, 404, "not found");
        });
        server.Get(R"(/.*)", [](const httplib::Request&, httplib::Response& response) {
            response.status = 404;
            response.set_content("not found", "text/plain; charset=utf-8");
        });
    }

    static void getJson(const std::function<std::string()>& callback, httplib::Response& response) {
        try {
            setJson(response, 200, callback());
        } catch (const std::exception& error) {
            setError(response, 500, error.what());
        } catch (...) {
            setError(response, 500, "internal error");
        }
    }

    std::uint16_t start() {
        if (running.load()) return boundPort;
        if (thread.joinable()) thread.join();
        const auto address = ParseIpv4Address(options.listenAddress);
        if (!address) throw std::runtime_error("Invalid web listen address");
        if (!IsValidAccessToken(options.accessToken)) {
            throw std::runtime_error("Invalid web access token: expected 16..256 printable ASCII characters");
        }
        token = options.accessToken;
        // The shared parser accepts decimal leading zeros; do not let getaddrinfo
        // reinterpret them as an octal address or as a DNS name.
        const auto host = canonicalIpv4(*address);
        const int port = options.port == 0 ? server.bind_to_any_port(host) :
            (server.bind_to_port(host, options.port) ? options.port : -1);
        if (port <= 0 || port > 65535) {
            server.stop(); // Reset httplib's decommissioned flag before a retry.
            server.closeUnstartedSocket();
            boundPort = 0;
            throw std::runtime_error("Failed to bind web server to " + options.listenAddress + ":" +
                std::to_string(options.port));
        }
        boundPort = static_cast<std::uint16_t>(port);
        running.store(true);
        try {
            thread = std::jthread([this] {
                try {
                    server.listen_after_bind();
                } catch (...) {
                    // Never let a listener-thread failure terminate the printing process.
                    server.closeUnstartedSocket();
                }
                running.store(false);
            });
        } catch (...) {
            running.store(false);
            server.stop();
            server.closeUnstartedSocket();
            boundPort = 0;
            throw;
        }
        // httplib's stop() must see a running listener, including when Stop()
        // follows Start() immediately without an intervening HTTP request.
        while (running.load() && !server.is_running()) std::this_thread::yield();
        if (!running.load()) {
            thread.join();
            server.stop();
            boundPort = 0;
            throw std::runtime_error("Failed to start web server listener");
        }
        return boundPort;
    }

    void stop(bool waitForRequests) {
        running.store(false);
        server.stop();
        if (thread.joinable()) {
            if (waitForRequests) thread.join();
            else thread.detach();
        }
        boundPort = 0;
    }

    Options options;
    HttpServer server;
    std::jthread thread;
    std::atomic<bool> running{false};
    std::string token;
    std::uint16_t boundPort = 0;
};

Server::Server(Options options) : options_(std::move(options)), impl_(std::make_unique<Impl>(options_)) {}
Server::~Server() { Stop(); }
std::uint16_t Server::Start() {
    const std::lock_guard lock(lifecycleMutex_);
    if (!impl_) impl_ = std::make_unique<Impl>(options_);
    return impl_->start();
}
void Server::Stop(bool waitForRequests) {
    const std::lock_guard lock(lifecycleMutex_);
    if (!impl_) return;
    impl_->stop(waitForRequests);
    if (waitForRequests) {
        options_ = std::move(impl_->options);
        impl_.reset();
    } else {
        // Final process shutdown only: httplib stop() closes the listening
        // socket, but its task queue joins workers still receiving requests.
        // Intentionally leak Impl (including callbacks) until the OS reclaims
        // the process. Captured callback state must also outlive these workers.
        // Tests and reusable servers must use the draining Stop()/destructor.
        (void)impl_.release();
    }
}
bool Server::IsRunning() const {
    const std::lock_guard lock(lifecycleMutex_);
    return impl_ && impl_->running.load();
}

} // namespace printhub::web
