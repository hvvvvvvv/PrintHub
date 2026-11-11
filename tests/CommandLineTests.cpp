#include <CommandLine.hpp>
#include <TextEncoding.h>

#include "TestSupport.h"

#include <Windows.h>
#include <shellapi.h>

#include <filesystem>
#include <memory>
#include <sstream>
#include <vector>

namespace {

using Arguments = std::vector<std::wstring>;
const std::filesystem::path kExecutablePath = L"C:\\Apps\\PrintHub.exe";

std::string requireInvalidArguments(const Arguments& arguments, std::string_view messagePrefix) {
    try {
        (void)ParseCommandLine(arguments, kExecutablePath);
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        require(message.starts_with(messagePrefix), "Unexpected command-line error: " + message);
        return message;
    }
    throw std::runtime_error("Invalid command line was accepted");
}

void testDefaultsAndPaths() {
    const auto defaults = ParseCommandLine({}, kExecutablePath);
    require(defaults.configPath == L"C:\\Apps\\config.json", "Default configuration is next to the executable");
    require(!defaults.serviceAction && !defaults.help && !defaults.version, "Default process options");
    require(!defaults.webBind && !defaults.webPort && !defaults.webToken,
        "Web defaults remain implicit so installation only persists explicit options");

    const std::filesystem::path absolutePath = L"C:\\Config Folder\\settings.json";
    require(ParseCommandLine({L"--config", absolutePath.native()}, kExecutablePath).configPath == absolutePath,
        "Absolute configuration path is unchanged");
    const std::filesystem::path normalizedPath = L"C:\\Config Folder\\subdir\\..\\settings.json";
    require(ParseCommandLine({L"--config", normalizedPath.native()}, kExecutablePath).configPath ==
        normalizedPath.lexically_normal(), "Absolute configuration path is lexically normalized");
    require(ParseCommandLine({L"--config", L"x.json"}, kExecutablePath).configPath ==
        std::filesystem::absolute(L"x.json").lexically_normal(), "Relative configuration uses the working directory");
    require(ParseCommandLine({L"--config", L"subdir\\..\\x.json"}, kExecutablePath).configPath ==
        std::filesystem::absolute(L"x.json").lexically_normal(), "Relative configuration path is normalized");

    const std::filesystem::path unicodePath = L"C:\\\u914d\u7f6e \u76ee\u5f55\\\u6253\u5370\u673a.json";
    require(ParseCommandLine({L"--config", unicodePath.native()}, kExecutablePath).configPath == unicodePath,
        "Unicode configuration path is preserved");
    const std::filesystem::path unicodeExecutable = L"C:\\\u6253\u5370\u670d\u52a1\\PrintHub.exe";
    require(ParseCommandLine({}, unicodeExecutable).configPath == unicodeExecutable.parent_path() / L"config.json",
        "Default configuration preserves the Unicode executable directory");
    const std::filesystem::path uncPath = L"\\\\server\\share\\config.json";
    require(ParseCommandLine({L"--config", uncPath.native()}, kExecutablePath).configPath == uncPath,
        "UNC configuration path is preserved");
}

void testConfigOptions() {
    const std::filesystem::path expected = L"C:\\Config Folder\\settings.json";
    const std::vector<Arguments> forms{
        {L"-c", expected.native()}, {L"--config", expected.native()},
        {L"--config=" + expected.native()}, {L"-c=" + expected.native()}
    };
    for (const auto& arguments : forms) {
        const auto parsed = ParseCommandLine(arguments, kExecutablePath);
        require(parsed.configPath == expected && !parsed.serviceAction, "All four config forms are equivalent");
    }
    for (const auto& first : forms) {
        for (const auto& second : forms) {
            auto duplicate = first;
            duplicate.insert(duplicate.end(), second.begin(), second.end());
            requireInvalidArguments(duplicate, "Duplicate option: --config");
        }
    }
    const std::vector<Arguments> missing{
        {L"--config"}, {L"-c"}, {L"--config="}, {L"-c="},
        {L"--config", L""}, {L"-c", L""},
        {L"--config", L"--service", L"start"}, {L"-c", L"--help"}
    };
    for (const auto& arguments : missing) requireInvalidArguments(arguments, "Missing value for ");
    const auto leadingDash = ParseCommandLine({L"--config=-settings.json"}, kExecutablePath);
    require(leadingDash.configPath == std::filesystem::absolute(L"-settings.json").lexically_normal(),
        "Inline values allow a filename beginning with a dash");
}

void testServiceActions() {
    const std::vector<std::pair<std::wstring, ServiceAction>> actions{
        {L"install", ServiceAction::Install}, {L"uninstall", ServiceAction::Uninstall},
        {L"start", ServiceAction::Start}, {L"stop", ServiceAction::Stop}, {L"restart", ServiceAction::Restart}
    };
    for (const auto& [name, action] : actions) {
        for (const auto& arguments : std::vector<Arguments>{{L"--service", name}, {L"--service=" + name}}) {
            const auto parsed = ParseCommandLine(arguments, kExecutablePath);
            require(parsed.serviceAction == action, "Both service forms parse every action");
            require(parsed.configPath == L"C:\\Apps\\config.json", "Service action retains the default path");
        }
        const std::string actionName = ServiceActionName(action);
        require(std::wstring(actionName.begin(), actionName.end()) == name, "Service action name");
        const auto before = ParseCommandLine({L"--config", L"x.json", L"--service", name}, kExecutablePath);
        const auto after = ParseCommandLine({L"--service=" + name, L"-c=x.json"}, kExecutablePath);
        require(before.serviceAction == action && after.serviceAction == action &&
            before.configPath == after.configPath, "Configuration can precede or follow the service action");
    }
    const std::vector<Arguments> missing{
        {L"--service"}, {L"--service="}, {L"--service", L""}, {L"--service", L"--help"}
    };
    for (const auto& arguments : missing) {
        const auto message = requireInvalidArguments(arguments, "Missing value for --service");
        for (const auto& entry : actions) {
            require(message.find(ServiceActionName(entry.second)) != std::string::npos, "Missing action lists legal values");
        }
    }
    for (const auto* value : {L"unknown", L"Install", L"START", L"start "}) {
        for (const auto& arguments : std::vector<Arguments>{{L"--service", value}, {std::wstring(L"--service=") + value}}) {
            const auto message = requireInvalidArguments(arguments, "Unknown service action: ");
            for (const auto& entry : actions) {
                require(message.find(ServiceActionName(entry.second)) != std::string::npos, "Unknown action lists legal values");
            }
        }
    }
    const std::vector<Arguments> duplicates{
        {L"--service", L"start", L"--service", L"stop"},
        {L"--service=start", L"--service=stop"},
        {L"--service", L"install", L"--service=install"},
        {L"--service=restart", L"--service", L"restart"}
    };
    for (const auto& arguments : duplicates) requireInvalidArguments(arguments, "Duplicate option: --service");
}

void testWebOptions() {
    const std::wstring token = L"ChangeMe-0123!=&/";
    const std::vector<Arguments> forms{
        {L"--web-bind", L"0.0.0.0", L"--web-port", L"8324", L"--web-token", token},
        {L"--web-bind=0.0.0.0", L"--web-port=8324", L"--web-token=" + token}
    };
    for (const auto& arguments : forms) {
        const auto parsed = ParseCommandLine(arguments, kExecutablePath);
        require(parsed.webBind == "0.0.0.0" && parsed.webPort == 8324 && parsed.webToken == WideToUtf8(token),
            "Spaced and inline web arguments preserve explicit values");
        require(parsed.configPath == L"C:\\Apps\\config.json" && !parsed.serviceAction,
            "Web arguments preserve configuration and service defaults");
    }
    std::wstring printableAscii;
    for (wchar_t character = 0x21; character <= 0x7e; ++character) printableAscii.push_back(character);
    for (const auto& value : std::vector<std::wstring>{
        std::wstring(16, L'!'), std::wstring(256, L'~'), printableAscii, L"-0123456789abcdef"}) {
        for (const auto& arguments : std::vector<Arguments>{{L"--web-token", value}, {L"--web-token=" + value}}) {
            require(ParseCommandLine(arguments, kExecutablePath).webToken == WideToUtf8(value),
                "Printable ASCII tokens, including leading dashes, are preserved verbatim");
        }
    }
    const std::string invalidTokenMessage =
        "Invalid value for --web-token: expected 16..256 printable ASCII characters";
    for (const auto& value : std::vector<std::wstring>{
        std::wstring(15, L'a'), std::wstring(257, L'a'), L"01234567 89abcdef",
        L"0123456789abcdef\u4ee4", L"0123456789abcdef\t", L"0123456789abcdef\n",
        L"0123456789abcdef\x7f", std::wstring(16, L'\0'), std::wstring(15, L'-'),
        std::wstring(16, static_cast<wchar_t>(0xd800))}) {
        for (const auto& arguments : std::vector<Arguments>{{L"--web-token", value}, {L"--web-token=" + value}}) {
            require(requireInvalidArguments(arguments, invalidTokenMessage) == invalidTokenMessage,
                "Invalid token diagnostic is stable");
        }
    }

    for (const auto* address : {L"127.0.0.1", L"255.255.255.255", L"010.001.002.003"}) {
        const auto parsed = ParseCommandLine({L"--web-bind", address}, kExecutablePath);
        require(parsed.webBind && std::wstring(parsed.webBind->begin(), parsed.webBind->end()) == address,
            "Valid IPv4 addresses retain their decimal spelling");
    }
    for (const auto* address : {L"256.1.1.1", L"::1", L"localhost", L"127.1", L"127.0.0.1 ", L"1.2.3.4.5"}) {
        for (const auto& arguments : std::vector<Arguments>{
            {L"--web-bind", address}, {std::wstring(L"--web-bind=") + address}}) {
            requireInvalidArguments(arguments, "Invalid value for --web-bind:");
        }
    }
    for (const auto port : {1, 65535}) {
        require(ParseCommandLine({L"--web-port", std::to_wstring(port)}, kExecutablePath).webPort == port,
            "Web port accepts both endpoints of 1..65535");
    }
    for (const auto* port : {L"0", L"65536", L"not-a-number", L"1.5", L"+80", L"80x", L" 80", L"80 ",
        L"999999999999999999999999999999999"}) {
        for (const auto& arguments : std::vector<Arguments>{
            {L"--web-port", port}, {std::wstring(L"--web-port=") + port}}) {
            requireInvalidArguments(arguments, "Invalid value for --web-port:");
        }
    }
    requireInvalidArguments({L"--web-port=-1"}, "Invalid value for --web-port:");

    const std::vector<std::pair<std::wstring, std::wstring>> options{
        {L"--web-bind", L"127.0.0.1"}, {L"--web-port", L"8323"}, {L"--web-token", token}
    };
    for (const auto& [option, value] : options) {
        const std::vector<Arguments> missing{
            {option}, {option + L"="}, {option, L""}, {option, L"--help"}
        };
        for (const auto& arguments : missing) {
            const auto message = "Missing value for " + WideToUtf8(option);
            require(requireInvalidArguments(arguments, message) == message, "Missing web value diagnostic is stable");
        }
        const std::vector<Arguments> spellings{{option, value}, {option + L"=" + value}};
        for (const auto& first : spellings) {
            for (const auto& second : spellings) {
                auto arguments = first;
                arguments.insert(arguments.end(), second.begin(), second.end());
                const auto message = "Duplicate option: " + WideToUtf8(option);
                require(requireInvalidArguments(arguments, message) == message, "Duplicate web option diagnostic is stable");
            }
        }
    }
    const auto combined = ParseCommandLine({L"--web-port=8324", L"--service", L"install", L"--config=config.json",
        L"--web-token", token, L"--web-bind", L"0.0.0.0"}, kExecutablePath);
    require(combined.serviceAction == ServiceAction::Install && combined.webPort == 8324 && combined.webBind == "0.0.0.0" &&
        combined.configPath == std::filesystem::absolute(L"config.json").lexically_normal() &&
        combined.webToken == WideToUtf8(token),
        "Web, config and service options can be freely interleaved");
}

void testInstallArguments() {
    require(BuildInstallArguments(ParseCommandLine({}, kExecutablePath)) ==
        Arguments{L"--config", L"C:\\Apps\\config.json"}, "Default installation persists only the configuration path");
    const auto full = ParseCommandLine({L"--web-token", L"ChangeMe-0123!=&/", L"--web-port=8324",
        L"--service=install", L"--web-bind=0.0.0.0", L"--config", L"C:\\Config Folder\\settings.json"}, kExecutablePath);
    require(BuildInstallArguments(full) == Arguments{L"--config", L"C:\\Config Folder\\settings.json",
        L"--web-bind", L"0.0.0.0", L"--web-port", L"8324", L"--web-token", L"ChangeMe-0123!=&/"},
        "Installation emits explicit web values in stable order without repeating the service action");
    require(BuildInstallArguments(ParseCommandLine({L"--web-bind=127.0.0.1"}, kExecutablePath)) ==
        Arguments{L"--config", L"C:\\Apps\\config.json", L"--web-bind", L"127.0.0.1"},
        "An explicit default bind address is persisted without other web defaults");
    require(BuildInstallArguments(ParseCommandLine({L"--web-port=8323"}, kExecutablePath)) ==
        Arguments{L"--config", L"C:\\Apps\\config.json", L"--web-port", L"8323"},
        "An explicit default port is persisted without other web defaults");
    require(BuildInstallArguments(ParseCommandLine({L"--web-token=ChangeMe-0123!=&/"}, kExecutablePath)) ==
        Arguments{L"--config", L"C:\\Apps\\config.json", L"--web-token", L"ChangeMe-0123!=&/"},
        "Token-only installation persists the original token string");
    require(BuildInstallArguments(ParseCommandLine({L"--web-port=9999", L"--web-bind=0.0.0.0"}, kExecutablePath)) ==
        Arguments{L"--config", L"C:\\Apps\\config.json", L"--web-bind", L"0.0.0.0", L"--web-port", L"9999"},
        "Partial web options do not add an implicit token");
}

void testUnknownOptionsAndHelp() {
    const std::vector<Arguments> unknown{
        {L"--unknown"}, {L"config.json"}, {L"--"}, {L""}, {L"--configx=x.json"},
        {L"--service-action=start"}, {L"--help=true"}, {L"--version=1"}, {L"-v"},
        {L"--raw-server", L"Printer"}, {L"--silent"}, {L"--config", L"x.json", L"extra"},
        {L"--help", L"--unknown"}
    };
    for (const auto& arguments : unknown) requireInvalidArguments(arguments, "Unknown option: ");
    const auto unicodeError = requireInvalidArguments({L"--\u672a\u77e5"}, "Unknown option: ");
    require(unicodeError == "Unknown option: --\xe6\x9c\xaa\xe7\x9f\xa5", "Unknown argument text is readable UTF-8");

    for (const auto* option : {L"-h", L"--help"}) {
        const auto parsed = ParseCommandLine({option}, kExecutablePath);
        require(parsed.help && !parsed.version && !parsed.serviceAction, "Help flag does not exit the parser");
    }
    const auto version = ParseCommandLine({L"--version"}, kExecutablePath);
    require(version.version && !version.help && !version.serviceAction, "Version flag does not exit the parser");
    std::ostringstream output;
    PrintCommandLineUsage(output);
    const auto usage = output.str();
    require(usage.find("--config") != std::string::npos && usage.find("--service") != std::string::npos,
        "Usage describes config and service options");
    require(usage.find("--help") != std::string::npos && usage.find("--version") != std::string::npos,
        "Usage describes help and version options");
    require(usage.find("--web-bind") != std::string::npos && usage.find("--web-port") != std::string::npos &&
        usage.find("--web-token <token>") != std::string::npos && usage.find("LAN example:") != std::string::npos,
        "Usage describes every web option and a LAN example");
    require(usage.find("--web-token-") == std::string::npos &&
        usage.find("default: random per start, printed to the log") != std::string::npos,
        "Usage describes a token value and the random startup default");
    require(usage.find("docs/configuration.md") != std::string::npos &&
        usage.find("missing file is created with default settings") != std::string::npos,
        "Usage points to configuration documentation and default creation");
}

void testWindowsQuoting() {
    const std::vector<std::pair<std::wstring, std::wstring>> examples{
        {L"plain", L"plain"}, {L"", L"\"\""}, {L"two words", L"\"two words\""},
        {L"a\tb", L"\"a\tb\""}, {L"a\"b", L"\"a\\\"b\""},
        {L"path\\", L"path\\"}, {L"two words\\", L"\"two words\\\\\""},
        {L"\u6253\u5370\u673A", L"\u6253\u5370\u673A"},
        {L"Office \u6253\u5370\u673A", L"\"Office \u6253\u5370\u673A\""}
    };
    for (const auto& [input, expected] : examples) {
        require(QuoteWindowsCommandLineArgument(input) == expected, "Exact Windows argument quoting");
    }
    for (size_t count = 0; count < 5; ++count) {
        const auto input = L"a" + std::wstring(count, L'\\') + L"\"b";
        const auto expected = L"\"a" + std::wstring(count * 2 + 1, L'\\') + L"\"b\"";
        require(QuoteWindowsCommandLineArgument(input) == expected, "Backslashes immediately before a quote");
    }
    require(BuildWindowsCommandLine({}).empty(), "Empty argument vector");
    require(BuildWindowsCommandLine({L"tool.exe", L"", L"two words", L"tail\\"}) ==
        L"tool.exe \"\" \"two words\" tail\\", "Exact command line spacing");

    // argv[0] has special Windows parsing rules; use a normal synthetic name.
    std::vector<std::wstring> arguments{L"test tool.exe"};
    for (const auto& [input, expected] : examples) arguments.push_back(input);
    arguments.push_back(L"a\\\\\"quoted\\");
    const auto command = BuildWindowsCommandLine(arguments);
    int count = 0;
    auto* parsed = CommandLineToArgvW(command.c_str(), &count);
    require(parsed != nullptr, "CommandLineToArgvW failed");
    struct LocalArgvDeleter {
        void operator()(wchar_t** value) const { LocalFree(value); }
    };
    const std::unique_ptr<wchar_t*, LocalArgvDeleter> owner(parsed);
    require(count == static_cast<int>(arguments.size()), "Windows round-trip argument count");
    for (int index = 0; index < count; ++index) {
        require(arguments[static_cast<size_t>(index)] == parsed[index], "Windows round-trip argument bytes");
    }
}

void testServiceCommandLineRoundTrip() {
    const std::filesystem::path executable = L"C:\\Program Files\\\u6253\u5370\\PrintHub.exe";
    const std::filesystem::path config = L"C:\\Config Folder\\\u914d\u7f6e.json";
    const std::wstring token = LR"(-0123456789!=&/\\"tail\)";
    const std::vector<Arguments> requests{
        {L"--config", config.native(), L"--service=install"},
        {L"--config", config.native(), L"--web-token", token,
            L"--service=install", L"--web-port=8324", L"--web-bind=0.0.0.0"},
        {L"--web-token=" + token, L"--config", config.native(), L"--service=install"}
    };
    for (const auto& request : requests) {
        const auto requested = ParseCommandLine(request, executable);
        auto installedArguments = BuildInstallArguments(requested);
        installedArguments.insert(installedArguments.begin(), executable.native());
        const auto command = BuildWindowsCommandLine(installedArguments);
        int count = 0;
        auto* parsed = CommandLineToArgvW(command.c_str(), &count);
        require(parsed != nullptr, "Service command line parsing failed");
        struct LocalArgvDeleter {
            void operator()(wchar_t** value) const { LocalFree(value); }
        };
        const std::unique_ptr<wchar_t*, LocalArgvDeleter> owner(parsed);
        require(count == static_cast<int>(installedArguments.size()) && executable == std::filesystem::path(parsed[0]),
            "Service executable path and argument count are preserved");
        Arguments reparsedArguments;
        for (int index = 1; index < count; ++index) reparsedArguments.emplace_back(parsed[index]);
        const auto options = ParseCommandLine(reparsedArguments, executable);
        require(options.configPath == config && !options.serviceAction,
            "Installed command line preserves the configuration and does not repeat installation");
        require(options.webBind == requested.webBind && options.webPort == requested.webPort &&
            options.webToken == requested.webToken,
            "Installed command line preserves web settings, token punctuation, quotes and backslashes");
    }
}

} // namespace

int main() {
    try {
        runTest("defaults and paths", testDefaultsAndPaths);
        runTest("config options", testConfigOptions);
        runTest("service actions", testServiceActions);
        runTest("web options", testWebOptions);
        runTest("install arguments", testInstallArguments);
        runTest("unknown options and help", testUnknownOptionsAndHelp);
        runTest("Windows quoting", testWindowsQuoting);
        runTest("service command line round trip", testServiceCommandLineRoundTrip);
    } catch (const std::exception& error) {
        std::cerr << "Command-line tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
