#include <PrintRuntime.h>

#include <ConfigurationFile.hpp>
#include <NetworkRuntime.h>
#include <PpmMib.h>
#include <PrinterMetadataCollector.h>
#include <Server.h>
#include <SnmpServer.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace {

constexpr auto kMetadataShutdownBudget = std::chrono::milliseconds(500);

spdlog::level::level_enum toSpdlogLevel(ServiceLogLevel level) {
    switch (level) {
    case ServiceLogLevel::Trace: return spdlog::level::trace;
    case ServiceLogLevel::Debug: return spdlog::level::debug;
    case ServiceLogLevel::Info: return spdlog::level::info;
    case ServiceLogLevel::Warn: return spdlog::level::warn;
    case ServiceLogLevel::Error: return spdlog::level::err;
    case ServiceLogLevel::Critical: return spdlog::level::critical;
    case ServiceLogLevel::Off: return spdlog::level::off;
    }
    throw std::invalid_argument("Unknown log level");
}

std::int64_t unixSecondsNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Called only from a catch block, including failures without a std::exception.
std::string currentError() {
    try {
        throw;
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "unexpected error";
    }
}

std::vector<std::unique_ptr<PrintServer>> createPrintServers(const ServiceConfiguration& configuration) {
    std::vector<std::unique_ptr<PrintServer>> servers;
    if (configuration.printerMap.empty()) {
        spdlog::info("No printers are configured; no print listeners will be started");
        return servers;
    }

    std::vector<LpdQueueMapping> queues;
    queues.reserve(configuration.printerMap.size());
    for (const auto& mapping : configuration.printerMap) {
        if (mapping.rawServer) {
            auto server = std::make_unique<RawPrinting>(mapping.name, mapping.rawServer->port,
                configuration.rawServerMinThreads, configuration.maxPendingClients);
            server->SetListenAddr(mapping.rawServer->listenAddress);
            servers.push_back(std::move(server));
        }
        queues.push_back({mapping.lprQueue, mapping.name});
    }
    auto lpdServer = std::make_unique<LPD>(configuration.lpdServer.port, std::move(queues),
        configuration.lpdMinThreads, ServiceConfiguration::kDefaultLpdMaxControlFileBytes,
        configuration.maxPendingClients);
    lpdServer->SetListenAddr(configuration.lpdServer.listenAddress);
    servers.push_back(std::move(lpdServer));
    return servers;
}

} // namespace

struct PrintRuntime::Impl {
    static constexpr std::size_t kMaxRetiredMetadataCollectors = 2;
    mutable std::mutex mutex_;
    // Keep Winsock alive until every socket and listener has been destroyed.
    std::optional<WinsockRuntime> winsock_;
    std::optional<ServiceConfiguration> configuration_;
    std::vector<std::unique_ptr<PrintServer>> servers_;
    std::optional<SnmpServer> snmpServer_;
    std::shared_ptr<ppm::PrinterMetadataCollector> metadataCollector_;
    std::vector<std::shared_ptr<ppm::PrinterMetadataCollector>> retiredCollectors_;
    bool running_ = false;
    bool stopping_ = false;
    bool lpdRunning_ = false;
    bool detachBypass_ = false;
    ApplyState applyState_ = ApplyState::Idle;
    std::atomic<ApplyState> publicApplyState_{ApplyState::Idle};
    std::string lastError_;
    std::int64_t lastAppliedUnixSeconds_ = 0;
    Snapshot cachedSnapshot_; // Updated only while holding mutex_.
    // A failed try_lock must not read cachedSnapshot_ while an apply publishes
    // it. Readers instead copy an immutable, atomically published generation.
    std::atomic<std::shared_ptr<const Snapshot>> publishedSnapshot_{nullptr};

    void setApplyStateLocked(ApplyState state) {
        applyState_ = state;
        publicApplyState_.store(state);
    }

    Snapshot snapshotLocked() const {
        Snapshot snapshot;
        snapshot.running = running_;
        snapshot.lpdRunning = lpdRunning_;
        snapshot.ppmRunning = snmpServer_.has_value() && snmpServer_->GetFailureCode() == 0;
        snapshot.applyState = applyState_;
        snapshot.lastError = lastError_;
        snapshot.lastAppliedUnixSeconds = lastAppliedUnixSeconds_;
        if (configuration_) {
            snapshot.lpdListenAddress = configuration_->lpdServer.listenAddress;
            snapshot.lpdPort = configuration_->lpdServer.port;
            snapshot.mappingCount = configuration_->printerMap.size();
            snapshot.ppmEnabled = !configuration_->ppmDisabled;
            if (running_) {
                for (const auto& mapping : configuration_->printerMap) {
                    if (mapping.rawServer) {
                        snapshot.rawListeners.push_back({mapping.name, mapping.rawServer->listenAddress,
                            mapping.rawServer->port});
                    }
                }
            }
        }
        return snapshot;
    }

    void cacheSnapshotLocked() {
        cachedSnapshot_ = snapshotLocked();
        publishedSnapshot_.store(std::make_shared<const Snapshot>(cachedSnapshot_));
    }

    void finishMetadataLocked() {
        if (!metadataCollector_) return;
        const bool stopped = metadataCollector_->StopUntil(ppm::Clock::now() + kMetadataShutdownBudget);
        metadataCollector_->LogDiagnostics();
        if (!stopped) {
            detachBypass_ = true;
            metadataCollector_->LogUnfinishedWorkers();
            spdlog::warn("Printer metadata collection is still blocked; retaining its collector and requiring DLL detach bypass at exit");
            retiredCollectors_.push_back(std::move(metadataCollector_));
        } else {
            metadataCollector_.reset();
        }
    }

    void stopComponentsLocked() {
        if (metadataCollector_) metadataCollector_->RequestStop();
        if (snmpServer_) {
            snmpServer_->Stop();
            spdlog::info("SNMP server stopped on {}:{}", configuration_->lpdServer.listenAddress, SnmpServer::PORT);
            snmpServer_.reset();
        }
        for (const auto& server : servers_) server->Stop();
        servers_.clear();
        lpdRunning_ = false;
        running_ = false;
        finishMetadataLocked();
    }

    void startPpmLocked() {
        if (retiredCollectors_.size() >= kMaxRetiredMetadataCollectors) {
            spdlog::error("PPM discovery disabled because previous metadata workers are still blocked; restart PrintService to recover");
            return;
        }
        if (configuration_->ppmDisabled) {
            spdlog::info("PPM discovery is disabled by configuration");
            return;
        }
        if (configuration_->printerMap.empty()) {
            spdlog::info("PPM discovery skipped because no printers are configured");
            return;
        }
        try {
            // Reserve the retirement slot before any worker starts, so bounded
            // shutdown does not need to allocate memory for a blocked collector.
            retiredCollectors_.reserve(retiredCollectors_.size() + 1);
            auto directory = std::make_shared<const PpmDirectory>(PpmMib::BuildDirectory(*configuration_));
            std::vector<std::wstring> queueNames;
            for (const auto& printer : directory->printers) queueNames.push_back(printer.queueName);
            metadataCollector_ = std::make_shared<ppm::PrinterMetadataCollector>(std::move(queueNames));
            snmpServer_.emplace(configuration_->lpdServer.listenAddress,
                [directory, collector = metadataCollector_](std::string_view request) {
                    const auto statuses = collector->GetStatuses();
                    const auto identities = collector->GetIdentities();
                    const auto mib = PpmMib::WithMetadata(*directory, *statuses, *identities, ppm::Clock::now());
                    return snmp::Respond(request, mib);
                },
                SnmpServer::PORT, [](int error) {
                    spdlog::error("PPM discovery disabled because SNMP failed with WSA error {}", error);
                });
            snmpServer_->Start();
            spdlog::info("SNMP PPM server started on {}:{}", configuration_->lpdServer.listenAddress, SnmpServer::PORT);
            spdlog::info("PPM discovery does not enable Windows SNMP status polling; check existing client port settings separately");
            metadataCollector_->Start();
        } catch (...) {
            const auto error = currentError();
            if (metadataCollector_) metadataCollector_->RequestStop();
            if (snmpServer_) {
                snmpServer_->Stop();
                snmpServer_.reset();
            }
            finishMetadataLocked();
            spdlog::error("PPM discovery disabled because SNMP could not start: {}", error);
        }
    }

    void startComponentsLocked() {
        configuration_->Validate();
        if (!winsock_) winsock_.emplace();
        servers_ = createPrintServers(*configuration_);
        for (const auto& server : servers_) {
            try {
                server->Start();
            } catch (...) {
                throw std::runtime_error(fmt::format("Failed to start print listener on {}:{}: {}",
                    server->GetListenAddr(), server->GetListenPort(), currentError()));
            }
            spdlog::info("Server started on {}:{}", server->GetListenAddr(), server->GetListenPort());
        }
        lpdRunning_ = !configuration_->printerMap.empty();
        startPpmLocked();
        running_ = true;
    }
};

PrintRuntime::PrintRuntime() : impl_(std::make_unique<Impl>()) {}

PrintRuntime::~PrintRuntime() { Stop(); }

void PrintRuntime::Start(const ServiceConfiguration& configuration) {
    std::lock_guard lock(impl_->mutex_);
    if (impl_->stopping_) throw std::runtime_error("Print runtime is shutting down");
    if (impl_->running_) throw std::runtime_error("Print runtime is already running");
    const auto previousLevel = spdlog::get_level();
    try {
        impl_->configuration_ = configuration;
        spdlog::set_level(toSpdlogLevel(configuration.logLevel));
        impl_->startComponentsLocked();
    } catch (...) {
        const auto error = currentError();
        impl_->stopComponentsLocked();
        impl_->winsock_.reset();
        spdlog::set_level(previousLevel);
        impl_->setApplyStateLocked(ApplyState::Error);
        impl_->lastError_ = error;
        impl_->cacheSnapshotLocked();
        throw std::runtime_error(error);
    }
    impl_->setApplyStateLocked(ApplyState::Idle);
    impl_->lastError_.clear();
    impl_->lastAppliedUnixSeconds_ = unixSecondsNow();
    impl_->cacheSnapshotLocked();
}

void PrintRuntime::Apply(const ServiceConfiguration& configuration) {
    std::lock_guard lock(impl_->mutex_);
    if (impl_->stopping_) throw std::runtime_error("Print runtime is shutting down");
    const bool wasRunning = impl_->running_;
    std::optional<ServiceConfiguration> previous;
    if (impl_->configuration_) previous = *impl_->configuration_;
    const auto previousLevel = spdlog::get_level();
    if (previous) {
        spdlog::debug("Applying configuration: mappings {} -> {}, LPD {}:{} -> {}:{}, PPM enabled {} -> {}, log level {} -> {}",
            previous->printerMap.size(), configuration.printerMap.size(),
            previous->lpdServer.listenAddress, previous->lpdServer.port,
            configuration.lpdServer.listenAddress, configuration.lpdServer.port,
            !previous->ppmDisabled, !configuration.ppmDisabled,
            ServiceConfiguration::LogLevelToString(previous->logLevel),
            ServiceConfiguration::LogLevelToString(configuration.logLevel));
    }
    impl_->setApplyStateLocked(ApplyState::Applying);
    impl_->lastError_.clear();
    impl_->cacheSnapshotLocked();
    try {
        if (wasRunning) impl_->stopComponentsLocked();
        impl_->configuration_ = configuration;
        spdlog::set_level(toSpdlogLevel(configuration.logLevel));
        impl_->startComponentsLocked();
    } catch (...) {
        const auto error = currentError();
        impl_->stopComponentsLocked();
        spdlog::set_level(previousLevel);
        bool restored = false;
        std::string rollbackError;
        if (previous) impl_->configuration_ = *previous;
        if (wasRunning) {
            try {
                impl_->startComponentsLocked();
                restored = true;
            } catch (...) {
                rollbackError = currentError();
                impl_->stopComponentsLocked();
            }
        }
        impl_->setApplyStateLocked(ApplyState::Error);
        impl_->lastError_ = error + (wasRunning
            ? (restored ? " (previous configuration restored)" : " (rollback failed: " + rollbackError + ")")
            : " (runtime is stopped; fix the configuration and save again to restart listeners)");
        impl_->cacheSnapshotLocked();
        spdlog::error("Configuration apply failed: {}", impl_->lastError_);
        throw std::runtime_error(impl_->lastError_);
    }
    impl_->setApplyStateLocked(ApplyState::Idle);
    impl_->lastError_.clear();
    impl_->lastAppliedUnixSeconds_ = unixSecondsNow();
    impl_->cacheSnapshotLocked();
    spdlog::info("Configuration applied: {} mapping(s), LPD {}:{}",
        impl_->configuration_->printerMap.size(), impl_->configuration_->lpdServer.listenAddress,
        impl_->configuration_->lpdServer.port);
}

void PrintRuntime::Stop() {
    std::lock_guard lock(impl_->mutex_);
    impl_->stopping_ = true;
    impl_->stopComponentsLocked();
    impl_->winsock_.reset();
    impl_->cacheSnapshotLocked();
}

ServiceConfiguration PrintRuntime::CopyConfiguration() const {
    std::lock_guard lock(impl_->mutex_);
    if (!impl_->configuration_) throw std::runtime_error("Print runtime has no configuration");
    return *impl_->configuration_;
}

std::string PrintRuntime::CurrentConfigJson() const {
    return SerializeConfigurationJson(CopyConfiguration());
}

PrintRuntime::Snapshot PrintRuntime::GetSnapshot() const {
    std::unique_lock lock(impl_->mutex_, std::try_to_lock);
    if (lock.owns_lock()) {
        impl_->cacheSnapshotLocked();
        return impl_->cachedSnapshot_;
    }
    const auto cached = impl_->publishedSnapshot_.load();
    auto snapshot = cached ? *cached : Snapshot{};
    snapshot.applyState = impl_->publicApplyState_.load();
    return snapshot;
}

bool PrintRuntime::HasActiveMetadata() const {
    std::lock_guard lock(impl_->mutex_);
    return impl_->metadataCollector_ || !impl_->retiredCollectors_.empty();
}

void PrintRuntime::PollDiagnostics() {
    std::vector<std::shared_ptr<ppm::PrinterMetadataCollector>> collectors;
    {
        std::lock_guard lock(impl_->mutex_);
        // StopUntil uses a deadline of now: never wait for a printer provider
        // while polling. Serialize collector lifetime operations with Apply.
        // The current collector keeps a timed-out detach marked unfinished;
        // the retirement cap also bounds those entries until process restart.
        for (auto item = impl_->retiredCollectors_.begin(); item != impl_->retiredCollectors_.end();) {
            if ((*item)->StopUntil(ppm::Clock::now())) {
                (*item)->LogDiagnostics();
                item = impl_->retiredCollectors_.erase(item);
            } else {
                ++item;
            }
        }
        collectors = impl_->retiredCollectors_;
        if (impl_->metadataCollector_) collectors.push_back(impl_->metadataCollector_);
    }
    for (const auto& collector : collectors) collector->LogDiagnostics();
}

bool PrintRuntime::NeedsDetachBypass() const {
    std::lock_guard lock(impl_->mutex_);
    return impl_->detachBypass_;
}
