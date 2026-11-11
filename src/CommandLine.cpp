#include <CommandLine.hpp>
#include <TextEncoding.h>

#include <stdexcept>

namespace {

constexpr auto kServiceActions = "install, uninstall, start, stop, restart";

std::string missingValueMessage(const std::wstring& option) {
    auto message = "Missing value for " + WideToUtf8(option);
    if (option == L"--service") message += std::string("; expected one of: ") + kServiceActions;
    return message;
}

const std::wstring& requireValue(const std::vector<std::wstring>& arguments, size_t& position,
    const std::wstring& option) {
    if (position + 1 >= arguments.size() || arguments[position + 1].empty() ||
        arguments[position + 1].front() == L'-') {
        throw std::invalid_argument(missingValueMessage(option));
    }
    return arguments[++position];
}

ServiceAction parseServiceAction(const std::wstring& value) {
    if (value == L"install") return ServiceAction::Install;
    if (value == L"uninstall") return ServiceAction::Uninstall;
    if (value == L"start") return ServiceAction::Start;
    if (value == L"stop") return ServiceAction::Stop;
    if (value == L"restart") return ServiceAction::Restart;
    throw std::invalid_argument("Unknown service action: " + WideToUtf8(value) +
        "; expected one of: " + kServiceActions);
}

} // namespace

const char* ServiceActionName(ServiceAction action) {
    switch (action) {
    case ServiceAction::Install: return "install";
    case ServiceAction::Uninstall: return "uninstall";
    case ServiceAction::Start: return "start";
    case ServiceAction::Stop: return "stop";
    case ServiceAction::Restart: return "restart";
    }
    throw std::invalid_argument("Unknown service action");
}

CommandLine ParseCommandLine(const std::vector<std::wstring>& arguments,
    const std::filesystem::path& executablePath) {
    CommandLine result;
    bool configSpecified = false;
    try {
        result.configPath = executablePath.parent_path() / L"config.json";
        for (size_t position = 0; position < arguments.size(); ++position) {
            const auto& argument = arguments[position];
            const auto separator = argument.find(L'=');
            const auto option = argument.substr(0, separator);
            if (option == L"-c" || option == L"--config" || option == L"--service") {
                const bool isConfig = option == L"-c" || option == L"--config";
                if (isConfig ? configSpecified : result.serviceAction.has_value()) {
                    throw std::invalid_argument(std::string("Duplicate option: ") +
                        (isConfig ? "--config" : "--service"));
                }
                const auto value = separator == std::wstring::npos
                    ? requireValue(arguments, position, option) : argument.substr(separator + 1);
                if (value.empty()) throw std::invalid_argument(missingValueMessage(option));
                if (isConfig) {
                    result.configPath = std::filesystem::path(value);
                    if (!result.configPath.is_absolute()) {
                        result.configPath = std::filesystem::absolute(result.configPath);
                    }
                    configSpecified = true;
                } else {
                    result.serviceAction = parseServiceAction(value);
                }
            } else if (argument == L"-h" || argument == L"--help") {
                result.help = true;
            } else if (argument == L"--version") {
                result.version = true;
            } else {
                throw std::invalid_argument("Unknown option: " + WideToUtf8(argument));
            }
        }
        result.configPath = result.configPath.lexically_normal();
    } catch (const std::filesystem::filesystem_error& error) {
        throw std::invalid_argument(std::string("Invalid configuration path: ") + error.what());
    }
    return result;
}

std::wstring QuoteWindowsCommandLineArgument(const std::wstring& value) {
    if (value.empty()) return L"\"\"";
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    size_t slashCount = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashCount;
        } else if (character == L'\"') {
            result.append(slashCount * 2 + 1, L'\\');
            result.push_back(character);
            slashCount = 0;
        } else {
            result.append(slashCount, L'\\');
            result.push_back(character);
            slashCount = 0;
        }
    }
    result.append(slashCount * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring BuildWindowsCommandLine(const std::vector<std::wstring>& arguments) {
    std::wstring result;
    for (const auto& argument : arguments) {
        if (!result.empty()) result.push_back(L' ');
        result += QuoteWindowsCommandLineArgument(argument);
    }
    return result;
}

void PrintCommandLineUsage(std::ostream& output) {
    output << R"(Usage:
  PrintHub.exe [--config <path>]
  PrintHub.exe [--config <path>] --service <install|uninstall|start|stop|restart>
  PrintHub.exe --help
  PrintHub.exe --version

Options:
  -c, --config <path>    Configuration file (default: <executable directory>\config.json)
      --service <action> Install, uninstall, start, stop or restart the PrintHub service
  -h, --help             Show this help
      --version          Show the PrintHub version

The configuration file is UTF-8 JSON; see docs/configuration.md and config.example.json.
)";
}
