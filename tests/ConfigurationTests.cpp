#include <ServiceConfiguration.hpp>
#include <ManagerCommands.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void requireThrows(const std::vector<std::string>& arguments) {
    try {
        static_cast<void>(ServiceConfiguration::Parse(arguments));
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid configuration was accepted");
}

void testRoundTrip() {
    const std::vector<std::string> input{
        "--raw-server", "Office Printer:9100", "--raw-server", "Second printer:9200",
        "--lpd-queue", "Office=Office Printer", "--lpd-queue", "Reports=Second printer",
        "--raw-server-min-threads", "2", "--lpd-port", "1515", "--listen-address", "192.168.1.20",
        "--snmp-reject", "--lpd-min-threads", "3", "--log-level", "debug"
    };
    const auto configuration = ServiceConfiguration::Parse(input);
    require(configuration.rawServers.size() == 2, "RAW entries were not parsed");
    require(configuration.rawServers.front().printerName == "Office Printer", "Printer name with spaces changed");
    require(configuration.rawServers[1].port == 9200, "RAW port changed");
    require(configuration.lpdQueues.size() == 2, "LPD queue entries were not parsed");
    require(configuration.lpdQueues[1].printerName == "Second printer", "LPD printer name with spaces changed");
    const auto roundTrip = ServiceConfiguration::Parse(configuration.ToArguments());
    require(roundTrip.rawServers.size() == 2 && roundTrip.lpdQueues.size() == 2, "Configuration did not round trip");
}

void testValidation() {
    requireThrows({ "--lpd-disable" });
    requireThrows({ "--lpd-queue", "queue" });
    requireThrows({ "--lpd-queue", "=Printer" });
    requireThrows({ "--lpd-queue", "queue=" });
    requireThrows({ "--lpd-queue", "Office=One", "--lpd-queue", "office=Two" });
    requireThrows({});
    requireThrows({ "--listen-address", "999.1.1.1" });
    requireThrows({ "--raw-server", "Printer:0" });
    requireThrows({ "--raw-server", "One:9100", "--raw-server", "Two:9100" });
    requireThrows({ "--raw-server-min-threads", "0" });
    requireThrows({ "--not-a-service-option" });
}

void testExplicitSerialization() {
    ServiceConfiguration configuration;
    configuration.lpdQueues.push_back({ "Office", "Office Printer" });
    const auto defaults = configuration.ToArguments(ServiceConfigurationArgumentFormat::Explicit);
    const std::vector<std::string> expectedDefaults{
        "--lpd-queue", "Office=Office Printer", "--raw-server-min-threads", "1", "--lpd-port", "515", "--listen-address", "0.0.0.0",
        "--lpd-min-threads", "1", "--log-level", "info"
    };
    require(defaults == expectedDefaults, "Explicit serialization omitted default values");

    configuration.rawServers.push_back({ "Office Printer", 9100 });
    configuration.snmpReject = true;
    const auto explicitInstall = BuildServiceManagerArguments(configuration, ServiceManagerAction::Install,
        ServiceConfigurationArgumentFormat::Explicit);
    require(std::find(explicitInstall.begin(), explicitInstall.end(), "--raw-server") != explicitInstall.end(),
        "Explicit install command omitted RAW mapping");
    require(std::find(explicitInstall.begin(), explicitInstall.end(), "--lpd-queue") != explicitInstall.end(),
        "Explicit install command omitted LPD queue mapping");
    require(std::find(explicitInstall.begin(), explicitInstall.end(), "--snmp-reject") != explicitInstall.end(),
        "Explicit install command omitted enabled SNMP flag");
    require(explicitInstall.size() >= expectedDefaults.size() + 4 && explicitInstall[explicitInstall.size() - 2] == "--service" &&
        explicitInstall.back() == "install", "Explicit install command omitted service action");
}

void testWindowsQuoting() {
    const auto commandLine = BuildWindowsCommandLine({ L"C:\\Program Files\\PrintHub.exe", L"--raw-server", L"A printer\\\"quoted:9100" });
    require(commandLine.find(L"\"C:\\Program Files\\PrintHub.exe\"") != std::wstring::npos, "Executable path was not quoted");
    require(commandLine.find(L"\\\\\\\"") != std::wstring::npos, "Embedded quote was not escaped");
}

void testManagerCommands() {
    ServiceConfiguration configuration;
    configuration.rawServers.push_back({ "Office Printer", 9100 });
    const auto install = BuildServiceManagerArguments(configuration, ServiceManagerAction::Install);
    require(install.size() == 4 && install[0] == "--raw-server" && install[2] == "--service" && install[3] == "install",
        "Install command did not include the serialized configuration");
    const auto update = BuildConfigurationUpdateSequence(true, true);
    require(update == std::vector<ServiceManagerAction>{ ServiceManagerAction::Stop, ServiceManagerAction::Uninstall,
        ServiceManagerAction::Install, ServiceManagerAction::Start }, "Running-service update sequence changed");
    require(BuildConfigurationUpdateSequence(false, false) == std::vector<ServiceManagerAction>{ ServiceManagerAction::Install,
        ServiceManagerAction::Start }, "Fresh-install sequence changed");
}

void testLpdQueueResolution() {
    const std::vector<LpdQueueMapping> mappings{
        { "Accounting", "Office Printer" }, { "Reports", "Office Printer" }
    };
    const auto resolved = ResolveLpdQueuePrinter(mappings, "aCcOuNtInG");
    require(resolved.has_value() && *resolved == "Office Printer", "LPD queue matching is not case-insensitive");
    require(!ResolveLpdQueuePrinter(mappings, "Office Printer").has_value(),
        "LPD resolver unexpectedly fell back to printer name");
}

} // namespace

int main() {
    try {
        testRoundTrip();
        testValidation();
        testExplicitSerialization();
        testWindowsQuoting();
        testManagerCommands();
        testLpdQueueResolution();
    } catch (const std::exception& error) {
        std::cerr << "Configuration test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
