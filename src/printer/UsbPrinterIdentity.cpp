#include <UsbPrinterIdentity.h>

#include <windows.h>
#include <winspool.h>
#include <setupapi.h>
#include <usbprint.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ppm {
namespace {
constexpr std::size_t kMaximumInfoBytes = 1024 * 1024;
constexpr std::size_t kMaximumUsbBytes = 4094;
constexpr DWORD kMaximumInterfaces = 256;
constexpr GUID kUsbPrintInterface{0x28d78fad, 0x5a12, 0x11d1, {0xae, 0x5b, 0, 0, 0xf8, 3, 0xa8, 0xc2}};

void Log(const MetadataDiagnostic& log, const std::string& text) { if (log) log(text); }
bool Equal(std::wstring_view a, std::wstring_view b) {
    return a.size() <= INT_MAX && b.size() <= INT_MAX &&
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
MetadataQueryResult Retry(std::uint32_t error) {
    return {QueryOutcome::Failed, 0, {}, error ? error : ERROR_GEN_FAILURE};
}
bool Usable(const UsbIdentityQueue& queue) {
    return queue.local && !(queue.attributes & PRINTER_ATTRIBUTE_WORK_OFFLINE) &&
        !(queue.status & (PRINTER_STATUS_OFFLINE | PRINTER_STATUS_SERVER_UNKNOWN |
            PRINTER_STATUS_NOT_AVAILABLE | PRINTER_STATUS_PENDING_DELETION));
}
bool UsbMonitor(const UsbIdentityQueue& queue) {
    if (Equal(queue.monitorDll, L"usbmon.dll")) return true;
    std::array<wchar_t, 32768> directory{};
    const auto length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    return length && length < directory.size() &&
        Equal(queue.monitorDll, std::wstring(directory.data(), length) + L"\\usbmon.dll");
}
std::optional<std::uint32_t> UsbPortNumber(std::wstring_view port) {
    if (port.size() < 4 || !Equal(port.substr(0, 3), L"USB")) return std::nullopt;
    std::uint32_t value = 0;
    for (const auto unit : port.substr(3)) {
        if (unit < L'0' || unit > L'9' || value > (UINT32_MAX - (unit - L'0')) / 10) return std::nullopt;
        value = value * 10 + unit - L'0';
    }
    return value == 0 ? std::nullopt : std::optional(value);
}
bool Matches(const UsbIdentityDevice& device, const UsbIdentityQueue& queue) {
    if (device.conflictingMapping || device.path.empty()) return false;
    if (!device.port.empty()) {
        if (!Equal(device.port, queue.port)) return false;
        return !device.portNumber || UsbPortNumber(device.port) == device.portNumber;
    }
    return device.portNumber && UsbPortNumber(queue.port) == device.portNumber;
}
bool SameQueue(const UsbIdentityQueue& a, const UsbIdentityQueue& b) {
    return Usable(b) && UsbMonitor(b) && Equal(a.port, b.port) && Equal(a.monitor, b.monitor);
}

struct Printer { HANDLE handle = nullptr; ~Printer() { if (handle) ClosePrinter(handle); } };
struct DeviceSet { HDEVINFO handle; ~DeviceSet() { if (handle != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(handle); } };
struct RegKey { HKEY handle; ~RegKey() { if (handle && handle != reinterpret_cast<HKEY>(INVALID_HANDLE_VALUE)) RegCloseKey(handle); } };
struct File { HANDLE handle; ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } };

// Winspool returns pointers into its buffer. Verify every string before copying.
bool BufferString(const wchar_t* pointer, const std::vector<BYTE>& buffer, std::wstring& text,
    std::size_t maximum = 32767) {
    if (!pointer) { text.clear(); return true; }
    const auto start = reinterpret_cast<std::uintptr_t>(buffer.data());
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (address < start || address - start >= buffer.size() || address % alignof(wchar_t)) return false;
    const auto available = (buffer.size() - (address - start)) / sizeof(wchar_t);
    const auto extent = (std::min)(available, maximum + 1);
    const auto length = wcsnlen_s(pointer, extent);
    if (length >= extent) return false;
    text.assign(pointer, length);
    return true;
}

template<class Read>
bool InfoBuffer(const Read& read, std::vector<BYTE>& buffer, std::uint32_t& error) {
    DWORD needed = 0;
    if (read(nullptr, 0, needed)) { buffer.clear(); error = 0; return true; }
    error = GetLastError();
    for (int attempt = 0; attempt < 3 && error == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        if (!needed || needed > kMaximumInfoBytes) { error = ERROR_INVALID_DATA; return false; }
        buffer.resize(needed);
        if (read(buffer.data(), static_cast<DWORD>(buffer.size()), needed)) {
            if (needed > buffer.size()) { error = ERROR_INVALID_DATA; return false; }
            error = 0; return true;
        }
        error = GetLastError();
    }
    return false;
}

class NativeUsbBackend final : public IUsbIdentityBackend {
public:
    NativeUsbBackend(std::stop_token stop, const MetadataDiagnostic& log) : stop_(stop), log_(log) {}
    bool Queue(std::wstring_view name, UsbIdentityQueue& result, std::uint32_t& error) override {
        if (Cancelled(error)) return false;
        Log(log_, "usb/queue: OpenPrinterW and GetPrinterW(level=2) starting");
        Printer printer;
        std::wstring queue(name);
        PRINTER_DEFAULTSW defaults{}; defaults.DesiredAccess = PRINTER_ACCESS_USE;
        if (!OpenPrinterW(queue.data(), &printer.handle, &defaults)) { error = GetLastError(); return false; }
        if (Cancelled(error)) return false;
        std::vector<BYTE> data;
        if (!InfoBuffer([&](BYTE* buffer, DWORD size, DWORD& needed) {
            return GetPrinterW(printer.handle, 2, buffer, size, &needed) != FALSE;
        }, data, error)) return false;
        if (data.size() < sizeof(PRINTER_INFO_2W)) { error = ERROR_INVALID_DATA; return false; }
        const auto info = reinterpret_cast<const PRINTER_INFO_2W*>(data.data());
        std::wstring server;
        if (!BufferString(info->pPortName, data, result.port, 1024) || !BufferString(info->pServerName, data, server)) {
            error = ERROR_INVALID_DATA; return false;
        }
        result.local = server.empty() && name.substr(0, 2) != L"\\\\";
        result.status = info->Status; result.attributes = info->Attributes;
        // No enumeration or hardware access is needed for a remote/pooled queue.
        if (!result.local || result.port.empty() || result.port.find(L',') != std::wstring::npos) return true;
        if (Cancelled(error)) return false;
        Log(log_, "usb/queue: resolving actual port monitor with EnumPortsW(level=2)");
        DWORD count = 0;
        if (!InfoBuffer([&](BYTE* buffer, DWORD size, DWORD& needed) {
            return EnumPortsW(nullptr, 2, buffer, size, &needed, &count) != FALSE;
        }, data, error)) return false;
        if (count > data.size() / sizeof(PORT_INFO_2W)) { error = ERROR_INVALID_DATA; return false; }
        const auto ports = reinterpret_cast<const PORT_INFO_2W*>(data.data());
        std::size_t matches = 0;
        for (DWORD index = 0; index < count; ++index) {
            std::wstring port, monitor;
            if (!BufferString(ports[index].pPortName, data, port, 1024) ||
                !BufferString(ports[index].pMonitorName, data, monitor, 256)) { error = ERROR_INVALID_DATA; return false; }
            if (Equal(port, result.port)) { result.monitor = std::move(monitor); ++matches; }
        }
        if (matches != 1) { error = matches ? ERROR_DUP_NAME : ERROR_UNKNOWN_PORT; return false; }
        if (Cancelled(error)) return false;
        if (!InfoBuffer([&](BYTE* buffer, DWORD size, DWORD& needed) {
            return EnumMonitorsW(nullptr, 2, buffer, size, &needed, &count) != FALSE;
        }, data, error)) return false;
        if (count > data.size() / sizeof(MONITOR_INFO_2W)) { error = ERROR_INVALID_DATA; return false; }
        const auto monitors = reinterpret_cast<const MONITOR_INFO_2W*>(data.data());
        matches = 0;
        for (DWORD index = 0; index < count; ++index) {
            std::wstring monitor, dll;
            if (!BufferString(monitors[index].pName, data, monitor, 256) ||
                !BufferString(monitors[index].pDLLName, data, dll, 1024)) { error = ERROR_INVALID_DATA; return false; }
            // PORT_INFO_2 may expose an alias ("Local Monitor", "TCPMON.DLL")
            // rather than EnumMonitors' registration name ("Local Port",
            // "Standard TCP/IP Port"). Resolve only explicit identities.
            if (MonitorRegistrationMatches(result.monitor, monitor, dll)) {
                Log(log_, "usb/monitor: port_label=" + MetadataDisplayText(result.monitor) +
                    "; registration=" + MetadataDisplayText(monitor) + "; dll=" + MetadataDisplayText(dll));
                result.monitorDll = std::move(dll); ++matches;
            }
        }
        if (matches != 1) { error = matches ? ERROR_DUP_NAME : ERROR_UNKNOWN_PRINT_MONITOR; return false; }
        return true;
    }

    bool Devices(std::vector<UsbIdentityDevice>& result, std::uint32_t& error) override {
        if (Cancelled(error)) return false;
        Log(log_, "usb/enumerate: SetupDiGetClassDevsW for present GUID_DEVINTERFACE_USBPRINT interfaces");
        DeviceSet devices{SetupDiGetClassDevsW(&kUsbPrintInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)};
        if (devices.handle == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
        for (DWORD index = 0; ; ++index) {
            if (Cancelled(error)) return false;
            SP_DEVICE_INTERFACE_DATA interfaceData{}; interfaceData.cbSize = sizeof(interfaceData);
            if (!SetupDiEnumDeviceInterfaces(devices.handle, nullptr, &kUsbPrintInterface, index, &interfaceData)) {
                error = GetLastError();
                if (error == ERROR_NO_MORE_ITEMS) { error = 0; return true; }
                return false;
            }
            if (index >= kMaximumInterfaces) { error = ERROR_BUFFER_OVERFLOW; return false; }
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(devices.handle, &interfaceData, nullptr, 0, &needed, nullptr);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)
                || needed > 65536) { error = ERROR_INVALID_DATA; return false; }
            std::vector<BYTE> detailBytes(needed);
            auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBytes.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA deviceInfo{}; deviceInfo.cbSize = sizeof(deviceInfo);
            if (!SetupDiGetDeviceInterfaceDetailW(devices.handle, &interfaceData, detail, needed, nullptr, &deviceInfo)) {
                error = GetLastError(); return false;
            }
            UsbIdentityDevice device;
            if (!BufferString(detail->DevicePath, detailBytes, device.path) || device.path.empty()) {
                error = ERROR_INVALID_DATA; return false;
            }
            // These are USBMON-maintained mappings, not an enumeration ordinal
            // or a decoded hardware/model identifier. Missing mappings fail closed.
            RegKey hardware{SetupDiOpenDevRegKey(devices.handle, &deviceInfo, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ)};
            if (hardware.handle != reinterpret_cast<HKEY>(INVALID_HANDLE_VALUE)) ReadMapping(hardware.handle, device, "device-key");
            else Log(log_, "usb/mapping: device-key open error=" + MetadataErrorText(GetLastError()));
            RegKey interfaceKey{SetupDiOpenDeviceInterfaceRegKey(devices.handle, &interfaceData, 0, KEY_READ)};
            if (interfaceKey.handle != reinterpret_cast<HKEY>(INVALID_HANDLE_VALUE)) ReadMapping(interfaceKey.handle, device, "interface-key");
            else Log(log_, "usb/mapping: interface-key open error=" + MetadataErrorText(GetLastError()));
            result.push_back(std::move(device));
        }
    }

    bool Read(const UsbIdentityDevice& device, std::vector<std::uint8_t>& result, std::uint32_t& error) override {
        if (Cancelled(error)) return false;
        Log(log_, "usb/open: CreateFileW read-only, shared read/write; path obtained from SetupAPI (not parsed)");
        File file{CreateFileW(device.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.handle == INVALID_HANDLE_VALUE) {
            error = GetLastError(); Log(log_, "usb/open: failed; error=" + MetadataErrorText(error)); return false;
        }
        std::array<BYTE, kMaximumUsbBytes> bytes{};
        DWORD returned = 0;
        if (Cancelled(error)) return false;
        Log(log_, "usb/ioctl: IOCTL_USBPRINT_GET_1284_ID starting; buffer=4094");
        if (!DeviceIoControl(file.handle, IOCTL_USBPRINT_GET_1284_ID, nullptr, 0, bytes.data(),
            static_cast<DWORD>(bytes.size()), &returned, nullptr)) {
            error = GetLastError(); Log(log_, "usb/ioctl: failed; error=" + MetadataErrorText(error)); return false;
        }
        Log(log_, "usb/ioctl: completed; returned_bytes=" + std::to_string(returned));
        if (returned > bytes.size()) { error = ERROR_INVALID_DATA; return false; }
        result.assign(bytes.begin(), bytes.begin() + returned);
        Log(log_, "usb/close: releasing the read-only device handle before decoding");
        return true;
    }

private:
    void MappingValues(HKEY key, UsbIdentityDevice& device, const std::string& source) {
        std::array<wchar_t, 128> name{};
        DWORD type = 0, size = sizeof(name);
        auto status = RegQueryValueExW(key, L"PortName", nullptr, &type, reinterpret_cast<BYTE*>(name.data()), &size);
        if (status == ERROR_SUCCESS) {
            const auto length = size / sizeof(wchar_t);
            Log(log_, "usb/mapping: " + source + " PortName present; type=" + std::to_string(type) + "; bytes=" + std::to_string(size));
            if (type != REG_SZ || size < sizeof(wchar_t) || size % sizeof(wchar_t) || length > name.size() ||
                name[length - 1] != 0 || wcsnlen_s(name.data(), length) != length - 1) device.conflictingMapping = true;
            else if (!device.port.empty() && !Equal(device.port, name.data())) device.conflictingMapping = true;
            else device.port = name.data();
        } else if (status != ERROR_FILE_NOT_FOUND) {
            device.conflictingMapping = true;
            Log(log_, "usb/mapping: " + source + " PortName error=" + MetadataErrorText(status));
        }
        // Only the USBMON interface key's numeric assignment is a logical
        // USBnnn print port. A device's unrelated physical USB/hub port number
        // must never select a printer.
        if (source.rfind("interface-key", 0) != 0) return;
        DWORD number = 0; size = sizeof(number);
        status = RegQueryValueExW(key, L"Port Number", nullptr, &type, reinterpret_cast<BYTE*>(&number), &size);
        if (status == ERROR_SUCCESS) {
            Log(log_, "usb/mapping: " + source + " Port Number present; type=" + std::to_string(type) +
                "; bytes=" + std::to_string(size) + "; value=" + std::to_string(number));
            if (type != REG_DWORD || size != sizeof(number) || !number ||
                (device.portNumber && *device.portNumber != number)) device.conflictingMapping = true;
            else device.portNumber = number;
        } else if (status != ERROR_FILE_NOT_FOUND) {
            device.conflictingMapping = true;
            Log(log_, "usb/mapping: " + source + " Port Number error=" + MetadataErrorText(status));
        }
    }
    void ReadMapping(HKEY key, UsbIdentityDevice& device, const std::string& source) {
        MappingValues(key, device, source);
        HKEY child = nullptr;
        const auto status = RegOpenKeyExW(key, L"Device Parameters", 0, KEY_QUERY_VALUE, &child);
        if (status == ERROR_SUCCESS) { RegKey cleanup{child}; MappingValues(child, device, source + "/Device Parameters"); }
        else if (status != ERROR_FILE_NOT_FOUND) {
            device.conflictingMapping = true;
            Log(log_, "usb/mapping: " + source + " Device Parameters error=" + MetadataErrorText(status));
        }
    }
    bool Cancelled(std::uint32_t& error) const noexcept {
        if (!stop_.stop_requested()) return false;
        error = ERROR_CANCELLED;
        return true;
    }
    std::stop_token stop_;
    const MetadataDiagnostic& log_;
};
} // namespace

bool MonitorRegistrationMatches(std::wstring_view portLabel, std::wstring_view registration, std::wstring_view dll) {
    return Equal(registration, portLabel) || Equal(dll, portLabel) ||
        (Equal(portLabel, L"Local Monitor") && Equal(registration, L"Local Port"));
}

std::string MetadataErrorText(std::uint32_t error) {
    std::ostringstream text;
    text << error << " (0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << error << ')';
    return text.str();
}

std::string MetadataDisplayText(std::wstring_view text) {
    if (text.size() > INT_MAX) return "<invalid-length>";
    if (text.empty()) return "\"\"";
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) return "<invalid-utf16>";
    std::string value(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(), size, nullptr, nullptr);
    std::string escaped = "\"";
    for (const unsigned char byte : value) {
        if (byte < 0x20 || byte == 0x7f) escaped += '?';
        else { if (byte == '"' || byte == '\\') escaped += '\\'; escaped += static_cast<char>(byte); }
    }
    return escaped + '"';
}

std::optional<std::string> DecodeUsbPrinterIdentity(std::span<const std::uint8_t> bytes, std::string* framing) {
    if (bytes.size() < 2 || bytes.size() > kMaximumUsbBytes) return std::nullopt;
    const auto declared = static_cast<std::size_t>(bytes[0]) * 256 + bytes[1];
    for (const bool inclusive : {true, false}) {
        if ((inclusive && declared < 2) || (!inclusive && declared == 0)) continue;
        const auto end = inclusive ? declared : declared + 2;
        if (end > bytes.size() || end < 2 ||
            !std::all_of(bytes.begin() + end, bytes.end(), [](auto byte) { return byte == 0; })) continue;
        auto payload = bytes.subspan(2, end - 2);
        if (!payload.empty() && payload.back() == 0) payload = payload.first(payload.size() - 1);
        std::string identity(reinterpret_cast<const char*>(payload.data()), payload.size());
        if (ValidateDeviceId(identity)) {
            if (framing) *framing = inclusive ? "big-endian length includes prefix" : "big-endian payload length";
            return identity;
        }
    }
    return std::nullopt;
}

MetadataQueryResult TryUsbIdentityFallback(std::wstring_view name, const MetadataQueryResult& bidi,
    IUsbIdentityBackend& backend, const MetadataDiagnostic& diagnostic) {
    if (bidi.outcome == QueryOutcome::Success && ValidateDeviceId(bidi.deviceId)) {
        Log(diagnostic, "identity/source: bidi; valid_id_bytes=" + std::to_string(bidi.deviceId.size()) + "; USB fallback not needed");
        return bidi;
    }
    Log(diagnostic, "usb/gate: no valid nonempty Bidi ID; bidi_outcome=" + std::to_string(static_cast<unsigned>(bidi.outcome)) +
        "; bidi_error=" + MetadataErrorText(bidi.errorCode));
    UsbIdentityQueue queue;
    std::uint32_t error = 0;
    if (!backend.Queue(name, queue, error)) {
        Log(diagnostic, "usb/skip: queue/monitor query failed; error=" + MetadataErrorText(error));
        return Retry(error);
    }
    Log(diagnostic, "usb/gate: port=" + MetadataDisplayText(queue.port) + "; monitor=" + MetadataDisplayText(queue.monitor) +
        "; monitor_dll=" + MetadataDisplayText(queue.monitorDll) + "; status=" + MetadataErrorText(queue.status) +
        "; attributes=" + MetadataErrorText(queue.attributes));
    if (!queue.local || queue.port.empty() || queue.port.find(L',') != std::wstring::npos) {
        Log(diagnostic, "usb/skip: queue is remote, has no port, or uses printer pooling; no device was opened");
        return bidi;
    }
    if (!UsbMonitor(queue)) {
        Log(diagnostic, "usb/skip: actual port monitor DLL is not usbmon.dll; no USB enumeration or I/O");
        return bidi;
    }
    if (!Usable(queue)) {
        Log(diagnostic, "usb/skip: queue is offline/work-offline/unavailable/unknown/deleting; retryable, no USB I/O");
        return Retry(ERROR_BIDI_DEVICE_OFFLINE);
    }
    std::vector<UsbIdentityDevice> devices;
    if (!backend.Devices(devices, error) || devices.size() > kMaximumInterfaces) {
        Log(diagnostic, "usb/fail: present-interface enumeration failed/over limit; error=" + MetadataErrorText(error));
        return Retry(error ? error : ERROR_BUFFER_OVERFLOW);
    }
    std::size_t matches = 0, selected = 0;
    for (std::size_t index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        const auto match = Matches(device, queue);
        Log(diagnostic, "usb/candidate: index=" + std::to_string(index) + "; PortName=" + MetadataDisplayText(device.port) +
            "; PortNumber=" + (device.portNumber ? std::to_string(*device.portNumber) : "absent") +
            "; mapping_conflict=" + (device.conflictingMapping ? "yes" : "no") + "; match=" + (match ? "yes" : "no"));
        if (match) { ++matches; selected = index; }
    }
    Log(diagnostic, "usb/match: present_interfaces=" + std::to_string(devices.size()) + "; matches=" + std::to_string(matches));
    if (matches != 1) {
        Log(diagnostic, "usb/skip: missing or ambiguous port-to-device mapping; no device was opened; retry in 60s");
        return Retry(matches ? ERROR_DUP_NAME : ERROR_NOT_FOUND);
    }
    UsbIdentityQueue checked;
    if (!backend.Queue(name, checked, error) || !SameQueue(queue, checked)) {
        Log(diagnostic, "usb/skip: queue changed or became offline before I/O; error=" + MetadataErrorText(error));
        return Retry(error ? error : ERROR_RETRY);
    }
    std::vector<std::uint8_t> bytes;
    if (!backend.Read(devices[selected], bytes, error)) {
        Log(diagnostic, "usb/fail: device open/read failed; error=" + MetadataErrorText(error) + "; retry in 60s");
        return Retry(error);
    }
    std::string framing;
    Log(diagnostic, "usb/decode: wire_bytes=" + std::to_string(bytes.size()) + "; declared_be_length=" +
        (bytes.size() >= 2 ? std::to_string(static_cast<unsigned>(bytes[0]) * 256 + bytes[1]) : "absent"));
    auto identity = DecodeUsbPrinterIdentity(bytes, &framing);
    if (!identity) {
        Log(diagnostic, "usb/fail: invalid framing/ASCII/MFG/MDL or ID exceeds 1023 bytes; returned_bytes=" + std::to_string(bytes.size()));
        return Retry(ERROR_INVALID_DATA);
    }
    if (!backend.Queue(name, checked, error) || !SameQueue(queue, checked)) {
        Log(diagnostic, "usb/discard: queue changed or became offline after I/O; identity not published");
        return Retry(error ? error : ERROR_RETRY);
    }
    Log(diagnostic, "identity/source: usb; valid_id_bytes=" + std::to_string(identity->size()) + "; framing=" + framing +
        "; cache until restart; ID contents omitted");
    return {QueryOutcome::Success, 0, std::move(*identity), 0};
}

MetadataQueryResult QueryUsbIdentityFallback(std::wstring_view queue,
    const MetadataQueryResult& bidi, std::stop_token stop, const MetadataDiagnostic& diagnostic) {
    NativeUsbBackend backend(stop, diagnostic);
    return TryUsbIdentityFallback(queue, bidi, backend, diagnostic);
}
} // namespace ppm
