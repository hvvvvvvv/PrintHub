#include <CommandLine.hpp>
#include <Ipv4.h>
#include <TextEncoding.h>

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <utility>

namespace {

constexpr auto kServiceActions = "install, uninstall, start, stop, restart";

std::string missingValueMessage(const std::wstring& option) {
    auto message = "Missing value for " + WideToUtf8(option);
    if (option == L"--service") message += std::string("; expected one of: ") + kServiceActions;
    return message;
}

const std::wstring& requireValue(const std::vector<std::wstring>& arguments, size_t& position,
    const std::wstring& option) {
    if (position + 1 >= arguments.size() || arguments[position + 1].empty()) {
        throw std::invalid_argument(missingValueMessage(option));
    }
    const auto& value = arguments[position + 1];
    // Tokens may start with '-' and must survive installation as a separate argv value.
    const bool namedOption = value == L"-c" || value == L"--config" || value == L"--service" ||
        value == L"--web-bind" || value == L"--web-port" || value == L"--web-token" ||
        value == L"-h" || value == L"--help" || value == L"--version";
    if (value.front() == L'-' && (option != L"--web-token" || namedOption)) {
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

std::filesystem::path resolvePath(const std::wstring& value, const char* description) {
    try {
        auto path = std::filesystem::path(value);
        if (!path.is_absolute()) path = std::filesystem::absolute(path);
        return path.lexically_normal();
    } catch (const std::filesystem::filesystem_error& error) {
        throw std::invalid_argument(std::string("Invalid ") + description + " path: " + error.what());
    }
}

std::uint16_t parseWebPort(const std::wstring& value) {
    const auto text = WideToUtf8(value);
    unsigned int port = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (error != std::errc{} || end != text.data() + text.size() || port == 0 || port > 65535) {
        throw std::invalid_argument("Invalid value for --web-port: expected an integer in 1..65535");
    }
    return static_cast<std::uint16_t>(port);
}

std::string parseWebToken(const std::wstring& value) {
    constexpr auto invalid = "Invalid value for --web-token: expected 16..256 printable ASCII characters";
    std::string token;
    try {
        token = WideToUtf8(value);
    } catch (const std::invalid_argument&) {
        throw std::invalid_argument(invalid);
    }
    if (token.size() < 16 || token.size() > 256 ||
        !std::all_of(token.begin(), token.end(), [](unsigned char character) {
            return character >= 0x21 && character <= 0x7e;
        })) {
        throw std::invalid_argument(invalid);
    }
    return token;
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
            if (option == L"-c" || option == L"--config" || option == L"--service" ||
                option == L"--web-bind" || option == L"--web-port" || option == L"--web-token") {
                const bool isConfig = option == L"-c" || option == L"--config";
                const bool duplicate = (isConfig && configSpecified) ||
                    (option == L"--service" && result.serviceAction.has_value()) ||
                    (option == L"--web-bind" && result.webBind.has_value()) ||
                    (option == L"--web-port" && result.webPort.has_value()) ||
                    (option == L"--web-token" && result.webToken.has_value());
                if (duplicate) {
                    throw std::invalid_argument(std::string("Duplicate option: ") +
                        (isConfig ? "--config" : WideToUtf8(option)));
                }
                const auto value = separator == std::wstring::npos
                    ? requireValue(arguments, position, option) : argument.substr(separator + 1);
                if (value.empty()) throw std::invalid_argument(missingValueMessage(option));
                if (isConfig) {
                    result.configPath = resolvePath(value, "configuration");
                    configSpecified = true;
                } else if (option == L"--service") {
                    result.serviceAction = parseServiceAction(value);
                } else if (option == L"--web-bind") {
                    auto address = WideToUtf8(value);
                    if (!ParseIpv4Address(address)) {
                        throw std::invalid_argument("Invalid value for --web-bind: expected an IPv4 address");
                    }
                    result.webBind = std::move(address);
                } else if (option == L"--web-port") {
                    result.webPort = parseWebPort(value);
                } else {
                    result.webToken = parseWebToken(value);
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

std::vector<std::wstring> BuildInstallArguments(const CommandLine& commandLine) {
    std::vector<std::wstring> result{L"--config", commandLine.configPath.native()};
    if (commandLine.webBind) {
        result.emplace_back(L"--web-bind");
        result.push_back(Utf8ToWide(*commandLine.webBind));
    }
    if (commandLine.webPort) {
        result.emplace_back(L"--web-port");
        result.push_back(std::to_wstring(*commandLine.webPort));
    }
    if (commandLine.webToken) {
        result.emplace_back(L"--web-token");
        result.push_back(Utf8ToWide(*commandLine.webToken));
    }
    return result;
}

void PrintCommandLineUsage(std::ostream& output) {
    output << R"(Usage:
  PrintHub.exe [--config <path>] [--web-bind <IPv4>] [--web-port <1-65535>] [--web-token <token>]
  PrintHub.exe [options] --service <install|uninstall|start|stop|restart>
  PrintHub.exe --help
  PrintHub.exe --version

Options:
  -c, --config <path>    Configuration file (default: <executable directory>\config.json)
      --service <action> Install, uninstall, start, stop or restart the PrintHub service
      --web-bind <IPv4>        Bind address for the web panel (default: 127.0.0.1)
      --web-port <1-65535>     Port for the web panel (default: 8323)
      --web-token <token>      Web panel access token (16-256 printable ASCII; default: random per start, printed to the log)
  -h, --help             Show this help
      --version          Show the PrintHub version

LAN example:
  PrintHub.exe --web-bind 0.0.0.0 --web-port 8323 --web-token "ChangeMe-0123456789"

The configuration file is UTF-8 JSON; see docs/configuration.md and config.example.json.
)";
}
