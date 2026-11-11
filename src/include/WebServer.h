#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace printhub::web {

bool IsValidAccessToken(std::string_view token);

struct ApplyResponse {
    int status = 500;
    std::string body = R"({"error":"internal error"})";
};

struct Options {
    std::string listenAddress = "127.0.0.1";
    std::uint16_t port = 8323;
    std::string accessToken;
    std::string version;
    std::function<std::string()> statusJson;
    std::function<std::string()> configJson;
    std::function<std::string(int tail)> logsJson;
    std::function<std::string()> printersJson;
    std::function<ApplyResponse(std::string_view body)> applyConfig;
};

class Server {
public:
    explicit Server(Options options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t Start();
    // false is only for final process shutdown: outstanding requests retain Impl.
    void Stop(bool waitForRequests = true);
    bool IsRunning() const;

private:
    struct Impl;
    mutable std::mutex lifecycleMutex_;
    Options options_; // Retain options so Start can follow a draining Stop.
    std::unique_ptr<Impl> impl_;
};

} // namespace printhub::web
