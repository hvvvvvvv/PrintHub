#include <ManagerCommands.hpp>

const char* ServiceManagerActionName(ServiceManagerAction action) {
    switch (action) {
    case ServiceManagerAction::Install: return "install";
    case ServiceManagerAction::Uninstall: return "uninstall";
    case ServiceManagerAction::Start: return "start";
    case ServiceManagerAction::Stop: return "stop";
    case ServiceManagerAction::Restart: return "restart";
    }
    return "unknown";
}

std::vector<std::string> BuildServiceManagerArguments(const ServiceConfiguration& configuration, ServiceManagerAction action,
    ServiceConfigurationArgumentFormat format) {
    std::vector<std::string> result;
    if (action == ServiceManagerAction::Install) {
        result = configuration.ToArguments(format);
    }
    result.push_back("--service");
    result.push_back(ServiceManagerActionName(action));
    return result;
}

std::vector<ServiceManagerAction> BuildConfigurationUpdateSequence(bool installed, bool running) {
    std::vector<ServiceManagerAction> result;
    if (installed) {
        if (running) result.push_back(ServiceManagerAction::Stop);
        result.push_back(ServiceManagerAction::Uninstall);
    }
    result.push_back(ServiceManagerAction::Install);
    result.push_back(ServiceManagerAction::Start);
    return result;
}
