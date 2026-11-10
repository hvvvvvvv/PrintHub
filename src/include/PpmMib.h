#pragma once

#include <ServiceConfiguration.hpp>
#include <SnmpProtocol.h>
#include <PrinterMetadata.h>
#include <span>

struct PpmPrinter {
    uint32_t index;
    std::wstring queueName;
};

struct PpmDirectory {
    std::vector<PpmPrinter> printers;
    snmp::Mib baseMib;
};

// Immutable installation metadata derived exclusively from the configured
// print listeners. No host printer enumeration or persisted indices are used.
class PpmMib {
public:
    static PpmDirectory BuildDirectory(const ServiceConfiguration& configuration);
    static snmp::Mib WithMetadata(const PpmDirectory& directory,
        std::span<const ppm::StatusSample> statuses,
        std::span<const ppm::IdentitySample> identities, ppm::Clock::time_point now);
};
