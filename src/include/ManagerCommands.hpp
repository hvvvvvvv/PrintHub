#pragma once

#include <ServiceConfiguration.hpp>

#include <string>
#include <vector>

enum class ServiceManagerAction {
    Install,
    Uninstall,
    Start,
    Stop,
    Restart,
};

const char* ServiceManagerActionName(ServiceManagerAction action);
std::vector<std::string> BuildServiceManagerArguments(const ServiceConfiguration& configuration, ServiceManagerAction action,
    ServiceConfigurationArgumentFormat format = ServiceConfigurationArgumentFormat::Compact);
std::vector<ServiceManagerAction> BuildConfigurationUpdateSequence(bool installed, bool running);
