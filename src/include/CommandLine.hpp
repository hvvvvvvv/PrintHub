#pragma once

#include <cstdint>
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
    std::optional<std::string> webBind;
    std::optional<std::uint16_t> webPort;
    std::optional<std::string> webToken;
    bool help = false;
    bool version = false;
};

CommandLine ParseCommandLine(const std::vector<std::wstring>& arguments,
    const std::filesystem::path& executablePath);
std::wstring QuoteWindowsCommandLineArgument(const std::wstring& value);
std::wstring BuildWindowsCommandLine(const std::vector<std::wstring>& arguments);
std::vector<std::wstring> BuildInstallArguments(const CommandLine& commandLine);
void PrintCommandLineUsage(std::ostream& output);
