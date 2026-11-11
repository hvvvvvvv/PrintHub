#include <ManagerCommands.hpp>

#include "TestSupport.h"

#include <vector>

namespace {

using Action = ServiceManagerAction;
using Format = ServiceConfigurationArgumentFormat;

void testActionNamesAndCommands() {
    ServiceConfiguration configuration;
    configuration.rawServers = {{"Office Printer", 9100}};
    configuration.lpdQueues = {{"Office", "Office Printer"}};
    configuration.snmpPpm = true;
    configuration.logLevel = ServiceLogLevel::Warn;
    const std::vector<std::string> compactInstall{
        "--raw-server", "Office Printer:9100", "--lpd-queue", "Office=Office Printer",
        "--snmp-ppm", "--log-level", "warn", "--service", "install"
    };
    const std::vector<std::string> explicitInstall{
        "--raw-server", "Office Printer:9100", "--lpd-queue", "Office=Office Printer",
        "--raw-server-min-threads", "1", "--max-pending-clients", "256", "--lpd-port", "515",
        "--lpd-max-control-file-bytes", "4194304", "--listen-address", "0.0.0.0",
        "--snmp-ppm", "--lpd-min-threads", "1", "--log-level", "warn", "--service", "install"
    };
    const std::vector<std::pair<Action, std::string>> actions{
        {Action::Install, "install"}, {Action::Uninstall, "uninstall"}, {Action::Start, "start"},
        {Action::Stop, "stop"}, {Action::Restart, "restart"}
    };
    for (const auto& [action, name] : actions) {
        require(ServiceManagerActionName(action) == name, "Action name: " + name);
        for (const auto format : {Format::Compact, Format::Explicit}) {
            const auto expected = action == Action::Install
                ? (format == Format::Compact ? compactInstall : explicitInstall)
                : std::vector<std::string>{"--service", name};
            require(BuildServiceManagerArguments(configuration, action, format) == expected,
                "Full manager argument sequence: " + name);
        }
        const auto expected = action == Action::Install ? compactInstall
            : std::vector<std::string>{"--service", name};
        require(BuildServiceManagerArguments(configuration, action) == expected, "Default format must be Compact");
    }
    require(std::string(ServiceManagerActionName(static_cast<Action>(99))) == "unknown", "Unknown action fallback");

    const ServiceConfiguration empty;
    for (const auto action : {Action::Uninstall, Action::Start, Action::Stop, Action::Restart}) {
        require(BuildServiceManagerArguments(empty, action) ==
            std::vector<std::string>{"--service", ServiceManagerActionName(action)},
            "Non-install actions must not require runnable configuration");
    }
    requireThrows([&] { BuildServiceManagerArguments(empty, Action::Install); },
        "Install must validate the configuration");
}

void testUpdateSequences() {
    require(BuildConfigurationUpdateSequence(false, false) == std::vector<Action>{Action::Install, Action::Start},
        "Not installed and stopped");
    require(BuildConfigurationUpdateSequence(false, true) == std::vector<Action>{Action::Install, Action::Start},
        "Running flag is ignored when not installed");
    require(BuildConfigurationUpdateSequence(true, false) ==
        std::vector<Action>{Action::Uninstall, Action::Install, Action::Start}, "Installed but stopped");
    require(BuildConfigurationUpdateSequence(true, true) ==
        std::vector<Action>{Action::Stop, Action::Uninstall, Action::Install, Action::Start}, "Installed and running");
}

} // namespace

int main() {
    try {
        runTest("action names and arguments", testActionNamesAndCommands);
        runTest("configuration update sequences", testUpdateSequences);
    } catch (const std::exception& error) {
        std::cerr << "Manager command tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
