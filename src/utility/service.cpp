#include<utility.h>
#include<fmt/format.h>

namespace utility {
	ServiceManager::ServiceManager(const std::string& serviceName) {
		m_serviceName = serviceName;
		if (m_serviceName.empty()) {
			throw std::invalid_argument("Service name cannot be empty");
		}
		m_serviceHandle = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
		if (!m_serviceHandle) {
			throw std::runtime_error(fmt::format("Failed to open service control manager. Error code: {}", GetLastError()));
		}
	}

	ServiceManager::~ServiceManager() {
		if (m_serviceHandle) {
			CloseServiceHandle(m_serviceHandle);
		}
	}

	bool ServiceManager::IsInstalled() const {
		auto service = OpenServiceA(m_serviceHandle, m_serviceName.c_str(), SERVICE_QUERY_STATUS);
		if (service) {
			CloseServiceHandle(service);
			return true;
		}
		return false;
	}

	bool ServiceManager::IsRunning() const {
		auto serviceHandle = OpenServiceA(m_serviceHandle, m_serviceName.c_str(), SERVICE_QUERY_STATUS);
		if (!serviceHandle) {
			return false;
		}
		SERVICE_STATUS status;
		if (!QueryServiceStatus(serviceHandle, &status)) {
			CloseServiceHandle(serviceHandle);
			return false;
		}
		CloseServiceHandle(serviceHandle);
		return status.dwCurrentState == SERVICE_RUNNING;
	}

	void ServiceManager::StartSrv() {

		auto serviceHandle = OpenServiceA(m_serviceHandle, m_serviceName.c_str(), SERVICE_START | SERVICE_QUERY_STATUS);
		if (!serviceHandle) {
			throw std::runtime_error(fmt::format("Failed to open service {}. Error code: {}", m_serviceName, GetLastError()));
		}
		if (!StartServiceA(serviceHandle, 0, nullptr)) {
			auto error = GetLastError();
			if (error != ERROR_SERVICE_ALREADY_RUNNING) {
				CloseServiceHandle(serviceHandle);
				throw std::runtime_error(fmt::format("Failed to start service {}. Error code: {}", m_serviceName, error));
			}
		}
		CloseServiceHandle(serviceHandle);
	}

	void ServiceManager::StopSrv() {
		auto serviceHandle = OpenServiceA(m_serviceHandle, m_serviceName.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
		if (!serviceHandle) {
			throw std::runtime_error(fmt::format("Failed to open service {}. Error code: {}", m_serviceName, GetLastError()));
		}
		SERVICE_STATUS status;
		if (!ControlService(serviceHandle, SERVICE_CONTROL_STOP, &status)) {
			auto error = GetLastError();
			if (error != ERROR_SERVICE_NOT_ACTIVE) {
				CloseServiceHandle(serviceHandle);
				throw std::runtime_error(fmt::format("Failed to stop service {}. Error code: {}", m_serviceName, error));
			}
		}
		CloseServiceHandle(serviceHandle);
	}

	void ServiceManager::RestartSrv() {
		StopSrv();
		StartSrv();
	}

	void ServiceManager::InstallSrv(const InstallArgs& args) {
		if (IsInstalled()) {
			throw std::runtime_error(fmt::format("Service {} is already installed.", m_serviceName));
		}
		if (args.binaryPath.empty()) {
			throw std::invalid_argument("Binary path cannot be empty");
		}
		std::vector<CHAR> deps;
		std::string binaryPath = fmt::format("\"{}\"", args.binaryPath);
		for(const auto& arg : args.arguments) {
			if (!arg.empty()) {
				binaryPath.append(" \"").append(arg).append("\"");
			}
		}	
		if(!args.dependencies.empty()) {
			for (const auto& dep : args.dependencies) {
				if (!dep.empty()) {
					deps.insert(deps.end(), dep.begin(), dep.end());
					deps.push_back('\0');
				}
			}	
		}
		deps.push_back('\0');
		auto serviceHandle = CreateServiceA(
			m_serviceHandle,
			m_serviceName.c_str(),
			args.displayName.value_or(m_serviceName).c_str(),
			SERVICE_ALL_ACCESS,
			SERVICE_WIN32_OWN_PROCESS,
			args.startAutomatic ? SERVICE_AUTO_START : SERVICE_DEMAND_START,
			SERVICE_ERROR_NORMAL,
			binaryPath.c_str(),
			nullptr, nullptr, 
			deps.data(),
			nullptr, nullptr);
		if (!serviceHandle) {
			throw std::runtime_error(fmt::format("Failed to create service {}. Error code: {}", m_serviceName, GetLastError()));
		}
		if (!args.description.has_value()) {
			CloseServiceHandle(serviceHandle);
			return;
		}
		SERVICE_DESCRIPTIONA desc = {0};
		desc.lpDescription = const_cast<LPSTR>(args.description->c_str());
		if (!ChangeServiceConfig2A(serviceHandle, SERVICE_CONFIG_DESCRIPTION, &desc)) {
			CloseServiceHandle(serviceHandle);
			throw std::runtime_error(fmt::format("Failed to set service description for {}. Error code: {}", m_serviceName, GetLastError()));
		}
		
		CloseServiceHandle(serviceHandle);
	}

	void ServiceManager::UninstallSrv() {
		auto serviceHandle = OpenServiceA(m_serviceHandle, m_serviceName.c_str(), SERVICE_STOP | DELETE);
		if (!serviceHandle) {
			throw std::runtime_error(fmt::format("Failed to open service {}. Error code: {}", m_serviceName, GetLastError()));
		}
		SERVICE_STATUS status;
		if (!ControlService(serviceHandle, SERVICE_CONTROL_STOP, &status)) {
			auto error = GetLastError();
			if(error != ERROR_SERVICE_NOT_ACTIVE) {
				CloseServiceHandle(serviceHandle);
				throw std::runtime_error(fmt::format("Failed to stop service {}. Error code: {}", m_serviceName, error));
			}
		}
		if (!DeleteService(serviceHandle)) {
			auto error = GetLastError();
			CloseServiceHandle(serviceHandle);
			throw std::runtime_error(fmt::format("Failed to delete service {}. Error code: {}", m_serviceName, error));
		}
		CloseServiceHandle(serviceHandle);
	}
}