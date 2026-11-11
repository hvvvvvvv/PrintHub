#include <ConfigurationFile.hpp>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>
#include <TextEncoding.h>
#include <Windows.h>

namespace {

using Json = nlohmann::json;

[[noreturn]] void invalidField(const std::string& field, const std::string& reason) {
    throw std::invalid_argument((field.empty() ? "$" : field) + ": " + reason);
}

std::string childField(const std::string& parent, const std::string& child) {
    return parent.empty() ? child : parent + "." + child;
}

void checkObject(const Json& value, const std::string& field,
    std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) invalidField(field, "expected object");
    for (auto item = value.begin(); item != value.end(); ++item) {
        if (std::find(allowed.begin(), allowed.end(), std::string_view(item.key())) == allowed.end()) {
            invalidField(childField(field, item.key()), "unknown field");
        }
    }
}

std::string readString(const Json& value, const std::string& field) {
    if (!value.is_string()) invalidField(field, "expected string");
    const auto& text = value.get_ref<const std::string&>();
    if (text.find('\0') != std::string::npos) invalidField(field, "embedded NUL is not allowed");
    try {
        return Utf8ToAcp(text);
    } catch (const std::invalid_argument& error) {
        invalidField(field, error.what());
    }
}

std::string readRequiredString(const Json& object, const char* name, const std::string& field) {
    const auto item = object.find(name);
    if (item == object.end()) invalidField(field, "required field");
    return readString(*item, field);
}

int readInteger(const Json& value, const std::string& field) {
    constexpr auto reason = "expected integer 1..65535";
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        if (number < 1 || number > UINT16_MAX) invalidField(field, reason);
        return static_cast<int>(number);
    }
    if (value.is_number_integer()) {
        const auto number = value.get<int64_t>();
        if (number < 1 || number > UINT16_MAX) invalidField(field, reason);
        return static_cast<int>(number);
    }
    invalidField(field, reason);
}

template <typename Server>
void readServer(const Json& value, const std::string& field, Server& server) {
    checkObject(value, field, {"listenAddress", "port"});
    if (const auto item = value.find("listenAddress"); item != value.end()) {
        server.listenAddress = readString(*item, field + ".listenAddress");
    }
    if (const auto item = value.find("port"); item != value.end()) {
        server.port = static_cast<uint16_t>(readInteger(*item, field + ".port"));
    }
}

class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : handle_(handle) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    HANDLE get() const { return handle_; }
    bool close() {
        if (handle_ == INVALID_HANDLE_VALUE) return true;
        if (!CloseHandle(handle_)) return false;
        handle_ = INVALID_HANDLE_VALUE;
        return true;
    }

private:
    HANDLE handle_;
};

class TemporaryFileCleanup {
public:
    explicit TemporaryFileCleanup(const std::filesystem::path& path) : path_(path) {}
    TemporaryFileCleanup(const TemporaryFileCleanup&) = delete;
    TemporaryFileCleanup& operator=(const TemporaryFileCleanup&) = delete;
    ~TemporaryFileCleanup() {
        if (active_) DeleteFileW(path_.c_str());
    }
    void release() { active_ = false; }

private:
    const std::filesystem::path& path_;
    bool active_ = true;
};

[[noreturn]] void fileError(const std::string& source, const char* operation, DWORD error) {
    throw std::runtime_error(source + ": " + operation + " failed (Win32 error " + std::to_string(error) + ")");
}

[[noreturn]] void fileTooLarge(const std::string& source) {
    throw std::runtime_error(source + ": configuration file exceeds 1 MiB (Win32 error " +
        std::to_string(ERROR_FILE_TOO_LARGE) + ")");
}

} // namespace

ServiceConfiguration ParseConfigurationJson(std::string_view json, std::string_view sourceName) {
    try {
        // A raw NUL is never JSON whitespace or a valid token terminator.
        if (json.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("invalid JSON: embedded NUL byte");
        }
        const auto root = Json::parse(json.begin(), json.end());
        checkObject(root, "", {"logLevel", "maxPendingClients", "lpdServer", "minThreads", "printerMap", "ppmDisabled"});
        ServiceConfiguration result;
        if (const auto item = root.find("logLevel"); item != root.end()) {
            const auto value = readString(*item, "logLevel");
            try {
                result.logLevel = ServiceConfiguration::ParseLogLevel(value);
            } catch (const std::invalid_argument& error) {
                invalidField("logLevel", error.what());
            }
        }
        if (const auto item = root.find("maxPendingClients"); item != root.end()) {
            result.maxPendingClients = readInteger(*item, "maxPendingClients");
        }
        if (const auto item = root.find("lpdServer"); item != root.end()) {
            readServer(*item, "lpdServer", result.lpdServer);
        }
        if (const auto item = root.find("minThreads"); item != root.end()) {
            checkObject(*item, "minThreads", {"raw", "lpd"});
            if (const auto raw = item->find("raw"); raw != item->end()) {
                result.rawServerMinThreads = readInteger(*raw, "minThreads.raw");
            }
            if (const auto lpd = item->find("lpd"); lpd != item->end()) {
                result.lpdMinThreads = readInteger(*lpd, "minThreads.lpd");
            }
        }
        if (const auto item = root.find("ppmDisabled"); item != root.end()) {
            if (!item->is_boolean()) invalidField("ppmDisabled", "expected boolean");
            result.ppmDisabled = item->get<bool>();
        }
        if (const auto item = root.find("printerMap"); item != root.end()) {
            if (!item->is_array()) invalidField("printerMap", "expected array");
            result.printerMap.reserve(item->size());
            for (size_t index = 0; index < item->size(); ++index) {
                const auto field = "printerMap[" + std::to_string(index) + "]";
                const auto& value = (*item)[index];
                checkObject(value, field, {"name", "lprQueue", "rawServer"});
                PrinterMapping mapping;
                mapping.name = readRequiredString(value, "name", field + ".name");
                mapping.lprQueue = readRequiredString(value, "lprQueue", field + ".lprQueue");
                if (const auto raw = value.find("rawServer"); raw != value.end()) {
                    mapping.rawServer.emplace();
                    readServer(*raw, field + ".rawServer", *mapping.rawServer);
                }
                result.printerMap.push_back(std::move(mapping));
            }
        }
        result.Validate();
        return result;
    } catch (const Json::parse_error& error) {
        throw std::invalid_argument(std::string(sourceName) + ": invalid JSON: " + error.what());
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string(sourceName) + ": invalid JSON: " + error.what());
    } catch (const std::invalid_argument& error) {
        throw std::invalid_argument(std::string(sourceName) + ": " + error.what());
    }
}

std::string SerializeConfigurationJson(const ServiceConfiguration& configuration) {
    configuration.Validate();
    Json mappings = Json::array();
    for (const auto& mapping : configuration.printerMap) {
        Json value = {
            {"name", AcpToUtf8(mapping.name)},
            {"lprQueue", AcpToUtf8(mapping.lprQueue)},
        };
        if (mapping.rawServer) {
            value["rawServer"] = {
                {"listenAddress", AcpToUtf8(mapping.rawServer->listenAddress)},
                {"port", mapping.rawServer->port},
            };
        }
        mappings.push_back(std::move(value));
    }
    const Json root = {
        {"logLevel", AcpToUtf8(ServiceConfiguration::LogLevelToString(configuration.logLevel))},
        {"maxPendingClients", configuration.maxPendingClients},
        {"lpdServer", {
            {"listenAddress", AcpToUtf8(configuration.lpdServer.listenAddress)},
            {"port", configuration.lpdServer.port},
        }},
        {"minThreads", {
            {"raw", configuration.rawServerMinThreads},
            {"lpd", configuration.lpdMinThreads},
        }},
        {"printerMap", std::move(mappings)},
        {"ppmDisabled", configuration.ppmDisabled},
    };
    return root.dump(2) + "\n";
}

ServiceConfiguration LoadConfigurationFile(const std::filesystem::path& path) {
    const auto source = WideToUtf8(path.native());
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) fileError(source, "CreateFileW", GetLastError());
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) fileError(source, "GetFileSizeEx", GetLastError());
    if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > kMaxConfigurationFileBytes) {
        fileTooLarge(source);
    }
    std::string content;
    content.reserve(static_cast<size_t>(size.QuadPart));
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            fileError(source, "ReadFile", GetLastError());
        }
        if (count == 0) break;
        // Recheck the limit because writers may grow a shared file after GetFileSizeEx.
        if (static_cast<size_t>(count) > kMaxConfigurationFileBytes - content.size()) fileTooLarge(source);
        content.append(buffer.data(), static_cast<size_t>(count));
    }
    if (!file.close()) fileError(source, "CloseHandle", GetLastError());
    if (content.compare(0, 3, "\xef\xbb\xbf") == 0) content.erase(0, 3);
    return ParseConfigurationJson(content, source);
}

ServiceConfiguration LoadOrCreateConfigurationFile(const std::filesystem::path& path, bool* created) {
    if (created) *created = false;
    try {
        return LoadConfigurationFile(path);
    } catch (const std::exception& loadError) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) throw;
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) throw;

        try {
            const auto parent = path.parent_path();
            if (!parent.empty()) {
                std::error_code directoryError;
                std::filesystem::create_directories(parent, directoryError);
                if (directoryError) {
                    throw std::runtime_error("Cannot create configuration directory '" +
                        WideToUtf8(parent.native()) + "': " + directoryError.message());
                }
            }
            ServiceConfiguration defaults;
            SaveConfigurationFile(path, defaults);
            if (created) *created = true;
            return defaults;
        } catch (const std::exception& createError) {
            throw std::runtime_error(std::string(loadError.what()) +
                "; could not create default configuration: " + createError.what());
        }
    }
}

void SaveConfigurationFile(const std::filesystem::path& path, const ServiceConfiguration& configuration) {
    const auto content = SerializeConfigurationJson(configuration);
    const auto source = WideToUtf8(path.native());
    if (content.size() > kMaxConfigurationFileBytes) fileTooLarge(source);
    const std::filesystem::path temporary(path.native() + L".tmp." + std::to_wstring(GetCurrentProcessId()));
    const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) fileError(source, "CreateFileW temporary file", GetLastError());
    // Destroy the handle before attempting deletion on every exception path.
    TemporaryFileCleanup cleanup(temporary);
    FileHandle file(handle);
    size_t offset = 0;
    while (offset < content.size()) {
        DWORD written = 0;
        const auto remaining = static_cast<DWORD>(content.size() - offset);
        if (!WriteFile(file.get(), content.data() + offset, remaining, &written, nullptr)) {
            fileError(source, "WriteFile", GetLastError());
        }
        if (written == 0) fileError(source, "WriteFile", ERROR_WRITE_FAULT);
        offset += static_cast<size_t>(written);
    }
    if (!FlushFileBuffers(file.get())) fileError(source, "FlushFileBuffers", GetLastError());
    if (!file.close()) fileError(source, "CloseHandle", GetLastError());
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fileError(source, "MoveFileExW", GetLastError());
    }
    cleanup.release();
}
