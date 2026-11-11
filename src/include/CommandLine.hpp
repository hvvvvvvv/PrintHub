#pragma once

#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

enum class ServiceAction { Install, Uninstall, Start, Stop, Restart };
const char* ServiceActionName(ServiceAction action);

struct CommandLine {
    std::filesystem::path configPath;
    std::optional<ServiceAction> serviceAction;
    bool help = false;
    bool version = false;
};

CommandLine ParseCommandLine(const std::vector<std::wstring>& arguments,
    const std::filesystem::path& executablePath);
std::wstring QuoteWindowsCommandLineArgument(const std::wstring& value);
std::wstring BuildWindowsCommandLine(const std::vector<std::wstring>& arguments);
void PrintCommandLineUsage(std::ostream& output);
