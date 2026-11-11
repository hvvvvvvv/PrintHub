#pragma once

#include <ServiceConfiguration.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class PrintRuntime {
public:
    enum class ApplyState { Idle, Applying, Error };
    struct RawListener {
        std::string printerName; // System ANSI code page, like ServiceConfiguration.
        std::string listenAddress;
        std::uint16_t port;
    };
    struct Snapshot {
        bool running = false;
        bool lpdRunning = false;
        std::string lpdListenAddress;
        std::uint16_t lpdPort = 0;
        std::size_t mappingCount = 0;
        std::vector<RawListener> rawListeners;
        bool ppmEnabled = false;
        bool ppmRunning = false;
        ApplyState applyState = ApplyState::Idle;
        std::string lastError;
        std::int64_t lastAppliedUnixSeconds = 0;
    };

    PrintRuntime();
    ~PrintRuntime();
    PrintRuntime(const PrintRuntime&) = delete;
    PrintRuntime& operator=(const PrintRuntime&) = delete;

    void Start(const ServiceConfiguration& configuration);
    void Apply(const ServiceConfiguration& configuration);
    void Stop();
    ServiceConfiguration CopyConfiguration() const;
    std::string CurrentConfigJson() const;
    Snapshot GetSnapshot() const;
    bool HasActiveMetadata() const;
    void PollDiagnostics();
    bool NeedsDetachBypass() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
