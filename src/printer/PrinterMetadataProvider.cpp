#include <PrinterMetadataProvider.h>
#include <UsbPrinterIdentity.h>

#include <windows.h>
#include <winspool.h>
#include <BiDiSpl.h>

#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace ppm {
namespace {

constexpr std::size_t kDeviceIdCapacity = 1024;
constexpr wchar_t kDeviceIdSchema[] = L"\\Printer.DeviceInfo:IEEE1284DeviceID";

QueryOutcome ErrorOutcome(std::uint32_t error) {
    const auto hr = static_cast<HRESULT>(error);
    const auto code = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : error;
    if (code == ERROR_INVALID_PRINTER_NAME || code == ERROR_PRINTER_NOT_FOUND || code == ERROR_FILE_NOT_FOUND)
        return QueryOutcome::NotFound;
    if (hr == E_NOTIMPL || hr == E_NOINTERFACE || hr == REGDB_E_CLASSNOTREG
        || code == ERROR_BIDI_NOT_SUPPORTED || code == ERROR_BIDI_SCHEMA_NOT_SUPPORTED)
        return QueryOutcome::Unsupported;
    return QueryOutcome::Failed;
}

MetadataQueryResult Failure(std::uint32_t error) {
    return {ErrorOutcome(error), 0, {}, error};
}

void ReportCleanup(const MetadataDiagnostic& diagnostic, std::string_view stage) noexcept {
    try { if (diagnostic) diagnostic(stage); } catch (...) {}
}

struct PrinterHandle {
    HANDLE value = nullptr;
    const MetadataDiagnostic& diagnostic;
    ~PrinterHandle() {
        if (!value) return;
        ReportCleanup(diagnostic, "status/ClosePrinter: starting");
        ClosePrinter(value);
    }
};

template<typename Reader>
MetadataQueryResult ReadStatusInformation(const Reader& read) {
    constexpr std::uint32_t maximumBytes = 64 * 1024;
    std::uint32_t needed = 0, error = 0;
    // The spooler can require padding beyond sizeof(PRINTER_INFO_6), including
    // eight bytes on current Windows. Always use its advertised buffer size.
    if (read(nullptr, 0, needed, error)) return Failure(ERROR_INVALID_DATA);
    if (error != ERROR_INSUFFICIENT_BUFFER) return Failure(error != 0 ? error : ERROR_GEN_FAILURE);
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (needed < sizeof(PRINTER_INFO_6) || needed > maximumBytes) return Failure(ERROR_INVALID_DATA);
        std::vector<std::uint8_t> buffer(needed);
        std::uint32_t actual = 0;
        error = 0;
        if (read(buffer.data(), static_cast<std::uint32_t>(buffer.size()), actual, error)) {
            if (actual < sizeof(PRINTER_INFO_6) || actual > buffer.size()) return Failure(ERROR_INVALID_DATA);
            std::uint32_t flags = 0;
            std::memcpy(&flags, buffer.data(), sizeof(flags));
            return {QueryOutcome::Success, flags, {}, 0, Clock::now()};
        }
        if (error != ERROR_INSUFFICIENT_BUFFER) return Failure(error != 0 ? error : ERROR_GEN_FAILURE);
        if (actual <= buffer.size() || actual > maximumBytes) return Failure(ERROR_INVALID_DATA);
        needed = actual;
    }
    return Failure(ERROR_INSUFFICIENT_BUFFER);
}

MetadataQueryResult ReadPrinterStatus(const std::wstring& queue,
    std::stop_token stop, const MetadataDiagnostic& diagnostic) {
    if (stop.stop_requested()) return Failure(ERROR_CANCELLED);
    PrinterHandle printer{nullptr, diagnostic};
    PRINTER_DEFAULTSW defaults{};
    defaults.DesiredAccess = PRINTER_ACCESS_USE;
    diagnostic("status/OpenPrinterW: starting");
    if (!OpenPrinterW(const_cast<wchar_t*>(queue.c_str()), &printer.value, &defaults)) return Failure(GetLastError());
    return ReadStatusInformation([&printer, stop, &diagnostic](std::uint8_t* buffer, std::uint32_t size,
        std::uint32_t& needed, std::uint32_t& error) {
        if (stop.stop_requested()) { error = ERROR_CANCELLED; return false; }
        diagnostic("status/GetPrinterW(level=6): starting");
        DWORD nativeNeeded = 0;
        const bool result = GetPrinterW(printer.value, 6, buffer, size, &nativeNeeded) != FALSE;
        error = result ? 0 : GetLastError();
        needed = nativeNeeded;
        return result;
    });
}

class ComApartment {
public:
    explicit ComApartment(const MetadataDiagnostic& diagnostic)
        : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), diagnostic_(diagnostic) {}
    ~ComApartment() {
        if (!SUCCEEDED(result)) return;
        ReportCleanup(diagnostic_, "bidi/CoUninitialize: starting");
        CoUninitialize();
    }
    HRESULT result;
private:
    const MetadataDiagnostic& diagnostic_;
};

template<typename T> class ComPointer {
public:
    explicit ComPointer(const MetadataDiagnostic& diagnostic) : diagnostic_(diagnostic) {}
    ~ComPointer() {
        if (!value_) return;
        ReportCleanup(diagnostic_, "bidi/Release: starting");
        value_->Release();
    }
    T* Get() const { return value_; }
    T** Out() { return &value_; }
private:
    T* value_ = nullptr;
    const MetadataDiagnostic& diagnostic_;
};

class BidiBinding {
public:
    BidiBinding(IBidiSpl* spl, const MetadataDiagnostic& diagnostic) : spl_(spl), diagnostic_(diagnostic) {}
    ~BidiBinding() {
        if (!bound) return;
        ReportCleanup(diagnostic_, "bidi/UnbindDevice: starting");
        spl_->UnbindDevice();
    }
    bool bound = false;
private:
    IBidiSpl* spl_;
    const MetadataDiagnostic& diagnostic_;
};

struct BidiOutput {
    LPWSTR schema = nullptr;
    BYTE* data = nullptr;
    ~BidiOutput() { CoTaskMemFree(schema); CoTaskMemFree(data); }
};

std::optional<MetadataQueryResult> CheckBidiResults(HRESULT sendResult,
    HRESULT getResultMethod, HRESULT requestResult) {
    if (sendResult != S_OK) return Failure(static_cast<std::uint32_t>(sendResult));
    if (getResultMethod != S_OK) return Failure(static_cast<std::uint32_t>(getResultMethod));
    if (requestResult != S_OK) return Failure(static_cast<std::uint32_t>(requestResult));
    return std::nullopt;
}

std::optional<MetadataQueryResult> DecodeIdentityOutput(std::wstring_view schema,
    DWORD type, std::string_view data) {
    if (schema.empty() || schema.size() >= 256) return Failure(ERROR_INVALID_DATA);
    if (CompareStringOrdinal(schema.data(), static_cast<int>(schema.size()), kDeviceIdSchema, -1, TRUE) != CSTR_EQUAL)
        return std::nullopt; // Another schema's result must not become this ID.
    if (type != BIDI_STRING || data.size() < sizeof(wchar_t)
        || data.size() > kDeviceIdCapacity * sizeof(wchar_t) || data.size() % sizeof(wchar_t) != 0)
        return Failure(ERROR_INVALID_DATA);
    const auto units = data.size() / sizeof(wchar_t);
    const auto readUnit = [&data](std::size_t index) {
        std::uint16_t value = 0;
        std::memcpy(&value, data.data() + index * sizeof(wchar_t), sizeof(value));
        return value;
    };
    if (readUnit(units - 1) != 0) return Failure(ERROR_INVALID_DATA);
    std::string candidate;
    candidate.reserve(units - 1);
    for (std::size_t unit = 0; unit + 1 < units; ++unit) {
        const auto value = readUnit(unit);
        if (value == 0 || value > 0x7F) return Failure(ERROR_INVALID_DATA);
        candidate.push_back(static_cast<char>(value));
    }
    if (!ValidateDeviceId(candidate)) return Failure(ERROR_INVALID_DATA);
    return MetadataQueryResult{QueryOutcome::Success, 0, std::move(candidate), 0};
}

MetadataQueryResult QueryBidiIdentity(const std::wstring& queue,
    std::stop_token stop, const MetadataDiagnostic& diagnostic) {
    const auto stage = [&](const char* name, HRESULT result) {
        diagnostic(std::string("bidi/") + name + ": HRESULT=" + MetadataErrorText(static_cast<std::uint32_t>(result)));
    };
    diagnostic("bidi/COM: initializing apartment and creating BidiSpl");
    ComApartment apartment(diagnostic);
    stage("CoInitializeEx", apartment.result);
    if (apartment.result != S_OK && apartment.result != S_FALSE) return Failure(static_cast<std::uint32_t>(apartment.result));
    if (stop.stop_requested()) return Failure(ERROR_CANCELLED);
    ComPointer<IBidiSpl> spl(diagnostic);
    HRESULT result = CoCreateInstance(__uuidof(BidiSpl), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IBidiSpl), reinterpret_cast<void**>(spl.Out()));
    stage("CoCreateInstance(BidiSpl)", result);
    if (result != S_OK) return Failure(static_cast<std::uint32_t>(result));
    if (stop.stop_requested()) return Failure(ERROR_CANCELLED);
    BidiBinding binding(spl.Get(), diagnostic);
    diagnostic("bidi/BindDevice: starting with BIDI_ACCESS_USER");
    result = spl.Get()->BindDevice(queue.c_str(), BIDI_ACCESS_USER);
    stage("BindDevice", result);
    if (result != S_OK) return Failure(static_cast<std::uint32_t>(result));
    binding.bound = true;
    if (stop.stop_requested()) return Failure(ERROR_CANCELLED);
    ComPointer<IBidiRequest> request(diagnostic);
    result = CoCreateInstance(__uuidof(BidiRequest), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IBidiRequest), reinterpret_cast<void**>(request.Out()));
    stage("CoCreateInstance(BidiRequest)", result);
    if (result != S_OK) return Failure(static_cast<std::uint32_t>(result));
    if (stop.stop_requested()) return Failure(ERROR_CANCELLED);
    result = request.Get()->SetSchema(kDeviceIdSchema);
    stage("SetSchema(IEEE1284DeviceID)", result);
    if (result != S_OK) return Failure(static_cast<std::uint32_t>(result));
    if (stop.stop_requested()) return Failure(ERROR_CANCELLED);
    diagnostic("bidi/SendRecv: Get IEEE1284DeviceID starting");
    const auto sendResult = spl.Get()->SendRecv(BIDI_ACTION_GET, request.Get());
    stage("SendRecv", sendResult);
    if (stop.stop_requested()) return Failure(ERROR_CANCELLED);
    HRESULT requestResult = E_FAIL;
    const auto getResultMethod = sendResult == S_OK ? request.Get()->GetResult(&requestResult) : E_FAIL;
    if (sendResult == S_OK) { stage("GetResult(method)", getResultMethod); stage("GetResult(request)", requestResult); }
    // The method HRESULT and the contained request result are separate. Some
    // print providers also return positive Win32/BIDI codes in the latter.
    if (auto failure = CheckBidiResults(sendResult, getResultMethod, requestResult)) return *failure;
    if (stop.stop_requested()) return Failure(ERROR_CANCELLED);
    DWORD count = 0;
    result = request.Get()->GetEnumCount(&count);
    stage("GetEnumCount", result);
    diagnostic("bidi/output: item_count=" + std::to_string(count));
    if (result != S_OK) return Failure(static_cast<std::uint32_t>(result));
    if (count == 0 || count > 64) return Failure(ERROR_INVALID_DATA);

    std::string identity;
    for (DWORD index = 0; index < count; ++index) {
        if (stop.stop_requested()) return Failure(ERROR_CANCELLED);
        BidiOutput output;
        DWORD type = BIDI_NULL;
        ULONG bytes = 0;
        result = request.Get()->GetOutputData(index, &output.schema, &type, &output.data, &bytes);
        stage("GetOutputData", result);
        diagnostic("bidi/output: index=" + std::to_string(index) + "; type=" + std::to_string(type) + "; bytes=" + std::to_string(bytes));
        if (result != S_OK) return Failure(static_cast<std::uint32_t>(result));
        if (!output.schema) return Failure(ERROR_INVALID_DATA);
        const auto schemaLength = wcsnlen_s(output.schema, 256);
        if (schemaLength == 256) return Failure(ERROR_INVALID_DATA);
        if (!output.data && bytes != 0) return Failure(ERROR_INVALID_DATA);
        const auto decoded = DecodeIdentityOutput(std::wstring_view(output.schema, schemaLength), type,
            output.data ? std::string_view(reinterpret_cast<const char*>(output.data), bytes) : std::string_view{});
        if (!decoded) continue;
        if (decoded->outcome != QueryOutcome::Success) {
            diagnostic("bidi/output: empty or invalid identity; contents omitted; USB fallback will be evaluated");
            return *decoded;
        }
        if (!identity.empty() && identity != decoded->deviceId)
            return Failure(ERROR_INVALID_DATA);
        identity = decoded->deviceId;
    }
    if (identity.empty()) return Failure(ERROR_INVALID_DATA);
    return {QueryOutcome::Success, 0, std::move(identity), 0};
}

MetadataQueryResult ReadPrinterIdentity(const std::wstring& queue,
    std::stop_token stop, const MetadataDiagnostic& diagnostic) {
    if (stop.stop_requested()) return Failure(ERROR_CANCELLED);
    const auto bidi = QueryBidiIdentity(queue, stop, diagnostic);
    if (stop.stop_requested()) return Failure(ERROR_CANCELLED);
    return QueryUsbIdentityFallback(queue, bidi, stop, diagnostic);
}

} // namespace

MetadataQueryResult QueryPrinterStatus(const std::wstring& queue,
    std::stop_token stop, const MetadataDiagnostic& diagnostic) {
    return ReadPrinterStatus(queue, stop, diagnostic);
}

MetadataQueryResult QueryPrinterIdentity(const std::wstring& queue,
    std::stop_token stop, const MetadataDiagnostic& diagnostic) {
    return ReadPrinterIdentity(queue, stop, diagnostic);
}

} // namespace ppm
