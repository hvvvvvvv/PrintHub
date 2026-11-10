#pragma once

#include <PrinterMetadata.h>

#include <array>
#include <memory>
#include <thread>
#include <vector>

namespace ppm {

class PrinterMetadataCollector {
public:
    using StatusSnapshot = std::shared_ptr<const std::vector<StatusSample>>;
    using IdentitySnapshot = std::shared_ptr<const std::vector<IdentitySample>>;

    explicit PrinterMetadataCollector(std::vector<std::wstring> queueNames);
    ~PrinterMetadataCollector();
    PrinterMetadataCollector(const PrinterMetadataCollector&) = delete;
    PrinterMetadataCollector& operator=(const PrinterMetadataCollector&) = delete;

    // Only the service thread manages worker lifetimes and drains diagnostics.
    void Start();
    void RequestStop() noexcept;
    // A false result requires the caller to bypass normal process teardown.
    bool StopUntil(Clock::time_point deadline) noexcept;
    void LogDiagnostics() const;
    void LogUnfinishedWorkers() const;

    StatusSnapshot GetStatuses() const noexcept;
    IdentitySnapshot GetIdentities() const noexcept;

private:
    struct State;
    std::shared_ptr<State> m_state;
    std::array<std::thread, 2> m_workers;
    unsigned m_unfinished = 0;
    bool m_started = false;
};

} // namespace ppm
