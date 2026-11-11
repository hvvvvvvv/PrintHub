#pragma once

#include <ServiceConfiguration.hpp>

#include <filesystem>
#include <string>
#include <string_view>

inline constexpr size_t kMaxConfigurationFileBytes = 1024 * 1024;

ServiceConfiguration ParseConfigurationJson(std::string_view json, std::string_view sourceName);
std::string SerializeConfigurationJson(const ServiceConfiguration& configuration);
ServiceConfiguration LoadConfigurationFile(const std::filesystem::path& path);
void SaveConfigurationFile(const std::filesystem::path& path, const ServiceConfiguration& configuration);
