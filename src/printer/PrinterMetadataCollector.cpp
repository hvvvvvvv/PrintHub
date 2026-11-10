#include <PrinterMetadataCollector.h>
#include <PrinterMetadataProvider.h>

#include <Windows.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <utility>

namespace ppm {
namespace {

constexpr auto kStatusInterval = std::chrono::seconds(5);
constexpr auto kIdentityRetry = std::chrono::seconds(60);
constexpr std::size_t kDiagnosticCount = 64;
constexpr std::size_t kDiagnosticLength = 511;
constexpr std::array<const char*, 2> kLaneNames{"status", "identity"};

struct DiagnosticLine {
    std::size_t printer = 0;
    std::array<char, kDiagnosticLength + 1> text{};
};

struct Diagnostics {
    std::mutex mutex;
    std::array<DiagnosticLine, kDiagnosticCount> lines{};
    std::size_t start = 0;
    std::size_t count = 0;
    DiagnosticLine last{};

    void Note(std::size_t printer, std::string_view text) noexcept {
        try {
            auto length = (std::min)(text.size(), kDiagnosticLength);
            if (length < text.size()) {
                while (length && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
            }
            DiagnosticLine line;
            line.printer = printer + 1;
            for (std::size_t i = 0; i < length; ++i) {
                const auto byte = static_cast<unsigned char>(text[i]);
                line.text[i] = byte < 0x20 || byte == 0x7f ? '?' : text[i];
            }
            std::lock_guard lock(mutex);
            lines[(start + count) % lines.size()] = line;
            if (count == lines.size()) start = (start + 1) % lines.size();
            else ++count;
            last = line;
        } catch (...) {
            // Diagnostics never make a failed query fatal or block its cleanup.
        }
    }
};

} // namespace

struct PrinterMetadataCollector::State {
    explicit State(std::vector<std::wstring> names)
        : queueNames(std::move(names)),
          statuses(std::make_shared<const std::vector<StatusSample>>(queueNames.size())),
          identities(std::make_shared<const std::vector<IdentitySample>>(queueNames.size())) {}

    const std::vector<std::wstring> queueNames;
    std::atomic<StatusSnapshot> statuses;
    std::atomic<IdentitySnapshot> identities;
    std::stop_source stop;
    std::mutex waitMutex;
    std::condition_variable_any wake;
    std::array<Diagnostics, 2> diagnostics;

    template<class Sample>
    void Run(std::atomic<std::shared_ptr<const std::vector<Sample>>>& cache,
        Diagnostics& trace) noexcept {
        constexpr bool isStatus = std::is_same_v<Sample, StatusSample>;
        auto stopToken = stop.get_token();
        std::size_t cursor = 0;
        try {
            if (queueNames.empty()) return;
            std::vector<Clock::time_point> due(queueNames.size(), (Clock::time_point::min)());
            while (!stopToken.stop_requested()) {
                const auto now = Clock::now();
                auto nextDue = (Clock::time_point::max)();
                std::size_t printer = queueNames.size();
                for (std::size_t offset = 0; offset < queueNames.size(); ++offset) {
                    const auto candidate = (cursor + offset) % queueNames.size();
                    if (due[candidate] <= now) { printer = candidate; break; }
                    nextDue = (std::min)(nextDue, due[candidate]);
                }
                if (printer == queueNames.size()) {
                    std::unique_lock lock(waitMutex);
                    if (nextDue == (Clock::time_point::max)()) {
                        wake.wait(lock, stopToken, [] { return false; });
                    } else {
                        wake.wait_until(lock, stopToken, nextDue, [] { return false; });
                    }
                    continue;
                }
                cursor = (printer + 1) % queueNames.size();
                if (stopToken.stop_requested()) return;
                const MetadataDiagnostic diagnostic = [&trace, printer](std::string_view line) noexcept {
                    trace.Note(printer, line);
                };
                diagnostic(isStatus ? "status/query: starting" : "identity/query: starting");

                MetadataQueryResult result;
                try {
                    // No wait, cache, or logging lock is held across Windows calls or cleanup.
                    if constexpr (isStatus) result = QueryPrinterStatus(queueNames[printer], stopToken, diagnostic);
                    else result = QueryPrinterIdentity(queueNames[printer], stopToken, diagnostic);
                } catch (...) {
                    result = {QueryOutcome::Failed, 0, {}, ERROR_GEN_FAILURE};
                    diagnostic("query raised an exception; treating it as a failed sample");
                }
                const auto completed = Clock::now();
                if (stopToken.stop_requested()) return;
                auto next = std::make_shared<std::vector<Sample>>(*cache.load());
                if constexpr (isStatus) {
                    (*next)[printer] = {result.outcome, result.flags, result.statusObservedAt};
                    due[printer] = completed + kStatusInterval;
                } else {
                    (*next)[printer] = {result.outcome, result.outcome == QueryOutcome::Success
                        ? std::move(result.deviceId) : std::string{}};
                    due[printer] = result.outcome == QueryOutcome::Success || result.outcome == QueryOutcome::Unsupported
                        ? (Clock::time_point::max)() : completed + kIdentityRetry;
                }
                if (stopToken.stop_requested()) return;
                cache.store(std::move(next));
                diagnostic("query completed: outcome=" + std::to_string(static_cast<unsigned>(result.outcome)) +
                    "; error=" + std::to_string(result.errorCode));
            }
        } catch (...) {
            // Preserve any published snapshot. Status samples will age out.
            trace.Note(cursor, "worker stopped after a local resource failure");
        }
    }
};

PrinterMetadataCollector::PrinterMetadataCollector(std::vector<std::wstring> queueNames)
    : m_state(std::make_shared<State>(std::move(queueNames))) {}

PrinterMetadataCollector::~PrinterMetadataCollector() {
    StopUntil(Clock::now());
}

void PrinterMetadataCollector::Start() {
    if (m_started || m_state->stop.stop_requested()) return;
    m_started = true;
    for (std::size_t lane = 0; lane < m_workers.size(); ++lane) {
        try {
            m_workers[lane] = std::thread([state = m_state, lane] {
                if (lane == 0) state->Run(state->statuses, state->diagnostics[lane]);
                else state->Run(state->identities, state->diagnostics[lane]);
            });
        } catch (const std::exception& error) {
            spdlog::warn("Printer {} worker could not start; continuing without its metadata: {}", kLaneNames[lane], error.what());
        }
    }
}

void PrinterMetadataCollector::RequestStop() noexcept {
    m_state->stop.request_stop();
}

bool PrinterMetadataCollector::StopUntil(Clock::time_point deadline) noexcept {
    RequestStop();
    for (std::size_t lane = 0; lane < m_workers.size(); ++lane) {
        auto& thread = m_workers[lane];
        if (!thread.joinable()) continue;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        const auto timeout = static_cast<DWORD>((std::clamp)(remaining.count(),
            decltype(remaining.count()){0}, decltype(remaining.count()){INFINITE - 1}));
        if (WaitForSingleObject(thread.native_handle(), timeout) == WAIT_OBJECT_0) {
            thread.join();
        } else {
            // The worker owns its entire State. The caller must not run DLL
            // detach while a printer API might still be holding driver locks.
            m_unfinished |= 1u << lane;
            thread.detach();
        }
    }
    return m_unfinished == 0;
}

PrinterMetadataCollector::StatusSnapshot PrinterMetadataCollector::GetStatuses() const noexcept {
    return m_state->statuses.load();
}

PrinterMetadataCollector::IdentitySnapshot PrinterMetadataCollector::GetIdentities() const noexcept {
    return m_state->identities.load();
}

void PrinterMetadataCollector::LogDiagnostics() const {
    for (std::size_t lane = 0; lane < m_state->diagnostics.size(); ++lane) {
        auto& trace = m_state->diagnostics[lane];
        std::array<DiagnosticLine, kDiagnosticCount> entries{};
        std::size_t count = 0;
        {
            std::unique_lock lock(trace.mutex, std::try_to_lock);
            if (!lock.owns_lock()) continue;
            count = trace.count;
            for (std::size_t i = 0; i < count; ++i) {
                entries[i] = trace.lines[(trace.start + i) % trace.lines.size()];
            }
            trace.count = 0;
            trace.start = 0;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto& entry = entries[i];
            spdlog::log(lane == 0 ? spdlog::level::debug : spdlog::level::info,
                "Printer {} index {}: {}", kLaneNames[lane], entry.printer, entry.text.data());
        }
    }
}

void PrinterMetadataCollector::LogUnfinishedWorkers() const {
    for (std::size_t lane = 0; lane < m_state->diagnostics.size(); ++lane) {
        if (!(m_unfinished & (1u << lane))) continue;
        auto& trace = m_state->diagnostics[lane];
        DiagnosticLine last;
        {
            std::unique_lock lock(trace.mutex, std::try_to_lock);
            if (lock.owns_lock()) last = trace.last;
        }
        spdlog::warn("Printer {} worker did not exit within the shutdown budget; index {}; last stage: {}",
            kLaneNames[lane], last.printer, last.text[0] ? last.text.data() : "unavailable");
    }
}

} // namespace ppm
