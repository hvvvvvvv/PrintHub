#pragma once

#include <PrinterMetadata.h>

#include <functional>
#include <stop_token>

namespace ppm {

struct MetadataQueryResult {
    QueryOutcome outcome = QueryOutcome::Failed;
    std::uint32_t flags = 0;
    std::string deviceId;
    std::uint32_t errorCode = 0;
    Clock::time_point statusObservedAt{}; // Stamped before potentially slow handle cleanup.
};

// Called by the owning worker, not by SNMP or the service control thread.
using MetadataDiagnostic = std::function<void(std::string_view)>;
MetadataQueryResult QueryPrinterStatus(const std::wstring& queue,
    std::stop_token stop, const MetadataDiagnostic& diagnostic);
MetadataQueryResult QueryPrinterIdentity(const std::wstring& queue,
    std::stop_token stop, const MetadataDiagnostic& diagnostic);

} // namespace ppm
