#pragma once

#include <ServiceConfiguration.hpp>

#include <filesystem>
#include <string>
#include <string_view>

inline constexpr size_t kMaxConfigurationFileBytes = 1024 * 1024;

ServiceConfiguration ParseConfigurationJson(std::string_view json, std::string_view sourceName);
std::string SerializeConfigurationJson(const ServiceConfiguration& configuration);
ServiceConfiguration LoadConfigurationFile(const std::filesystem::path& path);
// Loads an existing file; only a missing file/path triggers creation of parent
// directories and a default configuration with an empty printerMap. Existing
// unreadable/invalid files still throw. `created` is false unless creation succeeds.
ServiceConfiguration LoadOrCreateConfigurationFile(const std::filesystem::path& path, bool* created = nullptr);
void SaveConfigurationFile(const std::filesystem::path& path, const ServiceConfiguration& configuration);
