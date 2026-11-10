#pragma once

#include <PrinterMetadataProvider.h>
#include <optional>
#include <span>
#include <vector>

namespace ppm {

std::string MetadataErrorText(std::uint32_t error);
std::string MetadataDisplayText(std::wstring_view text);
bool MonitorRegistrationMatches(std::wstring_view portLabel, std::wstring_view registration,
    std::wstring_view dll);

struct UsbIdentityQueue {
    bool local = true;
    std::uint32_t status = 0, attributes = 0;
    std::wstring port, monitor, monitorDll;
};

struct UsbIdentityDevice {
    std::wstring path, port;
    std::optional<std::uint32_t> portNumber;
    bool conflictingMapping = false;
};

// Native implementation opens only present USBPRINT interfaces. Tests supply
// this interface without creating ports, changing queues or accessing hardware.
class IUsbIdentityBackend {
public:
    virtual ~IUsbIdentityBackend() = default;
    virtual bool Queue(std::wstring_view name, UsbIdentityQueue& result, std::uint32_t& error) = 0;
    virtual bool Devices(std::vector<UsbIdentityDevice>& result, std::uint32_t& error) = 0;
    virtual bool Read(const UsbIdentityDevice& device, std::vector<std::uint8_t>& result,
        std::uint32_t& error) = 0;
};

// Transport framing is removed, but the identity itself is never synthesized,
// localized or truncated. Both inclusive and payload-only length conventions
// are accepted only when all bytes outside the declared extent are zero.
std::optional<std::string> DecodeUsbPrinterIdentity(std::span<const std::uint8_t> bytes,
    std::string* framing = nullptr);
MetadataQueryResult TryUsbIdentityFallback(std::wstring_view queue,
    const MetadataQueryResult& bidi, IUsbIdentityBackend& backend,
    const MetadataDiagnostic& diagnostic);
MetadataQueryResult QueryUsbIdentityFallback(std::wstring_view queue,
    const MetadataQueryResult& bidi, std::stop_token stop, const MetadataDiagnostic& diagnostic);

} // namespace ppm
