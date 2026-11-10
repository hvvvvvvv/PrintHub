#include<utility.h>
#include<fmt/format.h>
#include <ServiceConfiguration.hpp>

#include <algorithm>

namespace utility {
	namespace {
		constexpr DWORD kNoServiceArguments = 0;
		constexpr DWORD kServiceStateTimeoutMs = 60000;
		constexpr DWORD kMinimumPollIntervalMs = 250;
		constexpr DWORD kMaximumPollIntervalMs = 1000;
		constexpr DWORD kRecoveryDelayMs = 10000;
		constexpr DWORD kRecoveryResetSeconds = 24 * 60 * 60;

		std::wstring toWide(const std::string& value) {
			if (value.empty()) return {};
			const int size = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, value.data(),
				static_cast<int>(value.size()), nullptr, 0);
			if (size <= 0) {
				throw std::runtime_error("Failed to convert service configuration text to Unicode");
			}
			std::wstring result(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, value.data(),
				static_cast<int>(value.size()), result.data(), size);
			return result;
		}

		void waitForServiceState(SC_HANDLE service, DWORD targetState, const char* action) {
			const auto deadline = GetTickCount64() + kServiceStateTimeoutMs;
			while (true) {
				SERVICE_STATUS_PROCESS status{};
				DWORD needed = 0;
				if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
					throw std::runtime_error(fmt::format("Failed to query service state while {}. Error code: {}", action, GetLastError()));
				}
				if (status.dwCurrentState == targetState) return;
				if (targetState == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) {
					throw std::runtime_error(fmt::format("Service stopped while starting. Win32 exit code: {}, service exit code: {}",
						status.dwWin32ExitCode, status.dwServiceSpecificExitCode));
				}
				if (GetTickCount64() >= deadline) {
					throw std::runtime_error(fmt::format("Timed out after {} ms while {}", kServiceStateTimeoutMs, action));
				}
				const auto hint = status.dwWaitHint == 0 ? kMaximumPollIntervalMs : status.dwWaitHint / 10;
				Sleep((std::clamp)(hint, kMinimumPollIntervalMs, kMaximumPollIntervalMs));
			}
		}

		void waitForServiceDeletion(SC_HANDLE manager, const std::string& serviceName) {
			const auto deadline = GetTickCount64() + kServiceStateTimeoutMs;
			while (true) {
				SC_HANDLE service = OpenServiceA(manager, serviceName.c_str(), SERVICE_QUERY_STATUS);
				if (!service) {
					const auto error = GetLastError();
					if (error == ERROR_SERVICE_DOES_NOT_EXIST) return;
					if (error != ERROR_SERVICE_MARKED_FOR_DELETE) {
						throw std::runtime_error(fmt::format("Failed to query service deletion. Error code: {}", error));
					}
				}
				CloseServiceHandle(service);
				if (GetTickCount64() >= deadline) {
					throw std::runtime_error(fmt::format("Timed out after {} ms while waiting for service deletion", kServiceStateTimeoutMs));
				}
				Sleep(kMinimumPollIntervalMs);
			}
		}

		void configureFailureRecovery(SC_HANDLE service) {
			SC_ACTION actions[3]{};
			for (auto& action : actions) {
				action.Type = SC_ACTION_RESTART;
				action.Delay = kRecoveryDelayMs;
			}
			SERVICE_FAILURE_ACTIONSA recovery{};
			recovery.dwResetPeriod = kRecoveryResetSeconds;
			recovery.cActions = 3;
			recovery.lpsaActions = actions;
			if (!ChangeServiceConfig2A(service, SERVICE_CONFIG_FAILURE_ACTIONS, &recovery)) {
				throw std::runtime_error(fmt::format("Failed to configure service recovery. Error code: {}", GetLastError()));
			}
			SERVICE_FAILURE_ACTIONS_FLAG recoveryFlag{ TRUE };
			if (!ChangeServiceConfig2A(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &recoveryFlag)) {
				throw std::runtime_error(fmt::format("Failed to enable service recovery for reported failures. Error code: {}", GetLastError()));
			}
		}
	}

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
		if (!StartServiceA(serviceHandle, kNoServiceArguments, nullptr)) {
			auto error = GetLastError();
			if (error != ERROR_SERVICE_ALREADY_RUNNING) {
				CloseServiceHandle(serviceHandle);
				throw std::runtime_error(fmt::format("Failed to start service {}. Error code: {}", m_serviceName, error));
			}
		}
		waitForServiceState(serviceHandle, SERVICE_RUNNING, "waiting for service startup");
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
			if (error != ERROR_SERVICE_NOT_ACTIVE && error != ERROR_SERVICE_CANNOT_ACCEPT_CTRL) {
				CloseServiceHandle(serviceHandle);
				throw std::runtime_error(fmt::format("Failed to stop service {}. Error code: {}", m_serviceName, error));
			}
		}
		waitForServiceState(serviceHandle, SERVICE_STOPPED, "waiting for service shutdown");
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
		// Convert to Unicode before quoting: CP_ACP multi-byte sequences may end
		// in 0x5C and must not be mistaken for command-line escape backslashes.
		std::vector<std::wstring> commandLineArguments{ toWide(args.binaryPath) };
		for (const auto& argument : args.arguments) {
			commandLineArguments.push_back(toWide(argument));
		}
		const auto binaryPath = BuildWindowsCommandLine(commandLineArguments);
		std::vector<wchar_t> deps;
		for (const auto& dep : args.dependencies) {
			if (dep.empty()) continue;
			const auto wideDep = toWide(dep);
			deps.insert(deps.end(), wideDep.begin(), wideDep.end());
			deps.push_back(L'\0');
		}
		deps.push_back(L'\0');
		const auto serviceName = toWide(m_serviceName);
		const auto displayName = toWide(args.displayName.value_or(m_serviceName));
		auto serviceHandle = CreateServiceW(
			m_serviceHandle,
			serviceName.c_str(),
			displayName.c_str(),
			SERVICE_ALL_ACCESS,
			SERVICE_WIN32_OWN_PROCESS,
			SERVICE_AUTO_START,
			SERVICE_ERROR_NORMAL,
			binaryPath.c_str(),
			nullptr, nullptr, 
			deps.data(),
			nullptr, nullptr);
		if (!serviceHandle) {
			throw std::runtime_error(fmt::format("Failed to create service {}. Error code: {}", m_serviceName, GetLastError()));
		}
		try {
			configureFailureRecovery(serviceHandle);
		}
		catch (...) {
			DeleteService(serviceHandle);
			CloseServiceHandle(serviceHandle);
			throw;
		}
		CloseServiceHandle(serviceHandle);
	}

	void ServiceManager::UninstallSrv() {
		StopSrv();
		auto serviceHandle = OpenServiceA(m_serviceHandle, m_serviceName.c_str(), DELETE);
		if (!serviceHandle) {
			throw std::runtime_error(fmt::format("Failed to open service {}. Error code: {}", m_serviceName, GetLastError()));
		}
		if (!DeleteService(serviceHandle)) {
			auto error = GetLastError();
			CloseServiceHandle(serviceHandle);
			throw std::runtime_error(fmt::format("Failed to delete service {}. Error code: {}", m_serviceName, error));
		}
		CloseServiceHandle(serviceHandle);
		waitForServiceDeletion(m_serviceHandle, m_serviceName);
	}
}
