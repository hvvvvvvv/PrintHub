#include <Server.h>
#include <winsvc.h>
#include <atomic>
#include <config.h>
#include <filesystem>
#include <minwindef.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/spdlog.h>
#include <utility.h>
#include <vector>
#include <conio.h>
#include <algorithm>
#include <Arguments.hpp>
#include <NetworkRuntime.h>
#include <SnmpRejectServer.h>

namespace {

constexpr auto kLogRotationHour = 0;
constexpr auto kLogRotationMinute = 0;
constexpr auto kLogRetentionDays = 7;
constexpr auto kLogFlushInterval = std::chrono::seconds(3);
constexpr DWORD kServiceStatusWaitHintMs = 15000;
constexpr DWORD kInitialServiceCheckpoint = 1;

} // namespace

std::atomic<bool> g_shutdown = false;
std::atomic<int> g_servExitCode = EXIT_SUCCESS;
SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
std::atomic<DWORD> g_serviceCheckpoint = kInitialServiceCheckpoint;
const std::unique_ptr<const Arguments> g_args = []() {
	auto args = std::make_unique<Arguments>();
	try {
		args->Parse(__argc, __argv);
	} catch (const std::exception& e) {
		spdlog::error("Failed to parse arguments: {}", e.what());
		args->PrintHelp();
		exit(EXIT_FAILURE);
	}
	return args;
}();

static void setupLogger(spdlog::level::level_enum level) {
	auto curDir = utility::getExecutableDirectory();
	auto& logDir = curDir.append("logs");
	auto logFile = fmt::format("{}.log", PROJECT_NAME);
	
	if (!std::filesystem::exists(logDir)) {
		std::filesystem::create_directories(logDir);
	}
	auto fileSink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(logDir.append(logFile).string(), kLogRotationHour, kLogRotationMinute, false, kLogRetentionDays);
	fileSink->set_level(spdlog::level::trace);
	spdlog::default_logger()->sinks().push_back(fileSink);

	spdlog::set_pattern("%Y-%m-%d %H:%M:%S.%e %l : %v");

	spdlog::set_level(level);

	spdlog::flush_every(kLogFlushInterval);
	
}

static void serviceActionHandle(const std::string& action) {
	auto serviceManager = utility::ServiceManager(PROJECT_NAME);
	if(action == Arguments::ARGS_SERVICE_INSTALL) {
		if (serviceManager.IsInstalled()) {
			throw std::runtime_error(fmt::format("Service {} is already installed.", PROJECT_NAME));
		}
		utility::ServiceManager::InstallArgs args;
		args.binaryPath = utility::getExecutablePath().string();
		args.arguments = g_args->GetRawArguments(std::set<std::string>{Arguments::ARGS_SERVICE});
		args.dependencies = { "Tcpip"};
		args.displayName = PROJECT_NAME;
		serviceManager.InstallSrv(args);
	} else if (action == Arguments::ARGS_SERVICE_UNINSTALL) {
		if (!serviceManager.IsInstalled()) {
			throw std::runtime_error(fmt::format("Service {} is not installed.", PROJECT_NAME));
		}
		serviceManager.UninstallSrv();
	} else if (action == Arguments::ARGS_SERVICE_START) {
		if (!serviceManager.IsInstalled()) {
			throw std::runtime_error(fmt::format("Service {} is not installed.", PROJECT_NAME));
		}
		if (serviceManager.IsRunning()) {
			throw std::runtime_error(fmt::format("Service {} is already running.", PROJECT_NAME));
		}
		serviceManager.StartSrv();
	} else if (action == Arguments::ARGS_SERVICE_STOP) {
		if (!serviceManager.IsInstalled()) {
			throw std::runtime_error(fmt::format("Service {} is not installed.", PROJECT_NAME));
		}
		if (!serviceManager.IsRunning()) {
			throw std::runtime_error(fmt::format("Service {} is not running.", PROJECT_NAME));
		}
		serviceManager.StopSrv();
	} else if (action == Arguments::ARGS_SERVICE_RESTART) {
		if(!serviceManager.IsInstalled()) {
			throw std::runtime_error(fmt::format("Service {} is not installed.", PROJECT_NAME));
		}
		serviceManager.RestartSrv();
	} else {
		throw std::invalid_argument(fmt::format("Unknown service action: {}", action));
	}
}

static std::vector<std::unique_ptr<PrintServer>> createRawServers() {
	auto servers = std::vector<std::unique_ptr<PrintServer>>();

	if(g_args->RawServersConfig.has_value()) {
		for (const auto& server : *g_args->RawServersConfig) {
			auto printerName = std::get<0>(server);
			auto port = std::get<1>(server);
			if (printerName.empty() || port == 0) {
				throw std::invalid_argument(fmt::format("Invalid raw server configuration: {}:{}", printerName, port));
			}
			servers.push_back(std::make_unique<RawPrinting>(printerName, port, g_args->RawServerMinThreads));
		}
	}
	if (!g_args->LpdDisable) {
		if (g_args->LpdPort == 0) {
			throw std::invalid_argument("LPD port cannot be zero");
		}
		if (g_args->LpdMinThreads < 1) {
			throw std::invalid_argument("LPD minimum threads must be at least 1");
		}
		servers.push_back(std::make_unique<LPD>(g_args->LpdPort, g_args->LpdMinThreads));
	}
	return servers;
}

static BOOL WINAPI consoleHandler(DWORD ctrlType) {
	switch (ctrlType) {
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
		case CTRL_CLOSE_EVENT:
		case CTRL_LOGOFF_EVENT:
		case CTRL_SHUTDOWN_EVENT:
			spdlog::info("Received console control signal: {}", ctrlType);
			g_shutdown.store(true);
			g_shutdown.notify_all();
			return TRUE;
		default:
			return FALSE;
	}
}

static void reportServiceStatus(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD serviceExitCode = 0, DWORD waitHint = 0) {
	if (!g_serviceStatusHandle) {
		return;
	}
	SERVICE_STATUS status{};
	status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	status.dwCurrentState = state;
	status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
	status.dwWin32ExitCode = win32ExitCode;
	status.dwServiceSpecificExitCode = serviceExitCode;
	status.dwWaitHint = waitHint;
	status.dwCheckPoint = (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
		? g_serviceCheckpoint.fetch_add(1) : 0;
	SetServiceStatus(g_serviceStatusHandle, &status);
}

static DWORD WINAPI svcCtrlHandler(DWORD dwControl, DWORD, LPVOID, LPVOID) {
	switch (dwControl) {
		case SERVICE_CONTROL_STOP:
		case SERVICE_CONTROL_SHUTDOWN: {
			reportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 0, kServiceStatusWaitHintMs);
			g_shutdown.store(true);
			g_shutdown.notify_all();
			spdlog::info("Service stopping...");
			return NO_ERROR;
		} break;
		case SERVICE_CONTROL_INTERROGATE: {
			return NO_ERROR;
		} break;
		default: {
			return ERROR_CALL_NOT_IMPLEMENTED;
		}
	}
}

static int mainRun() {
	spdlog::default_logger()->info("Starting {} version {}", PROJECT_NAME, PROJECT_VERSION);
	spdlog::info("Build date: {}", __DATE__);
	spdlog::info("Build time: {}", __TIME__);

	try {
		WinsockRuntime winsockRuntime;
		auto servers = createRawServers();
		if (servers.empty()) {
			spdlog::warn("No servers configured, exiting.");
			return 1;
		}
		for (auto& server : servers) {
			server->SetListenAddr(g_args->ListenAddress);
			server->Start();
			spdlog::info("Server started on {}:{}", server->GetListenAddr(), server->GetListenPort());
		}
		std::optional<SnmpRejectServer> snmpRejectServer;
		if (g_args->SnmpReject) {
			snmpRejectServer.emplace(g_args->ListenAddress);
			snmpRejectServer->Start();
			spdlog::info("SNMP reject server started on {}:{}", g_args->ListenAddress, SnmpRejectServer::PORT);
		}

		while (!g_shutdown.load()) {
			g_shutdown.wait(false);
		}
		for (auto& server : servers) {
			server->Stop();
			spdlog::info("server stopped on {}:{}", server->GetListenAddr(), server->GetListenPort());
		}
		if (snmpRejectServer.has_value()) {
			snmpRejectServer->Stop();
			spdlog::info("SNMP reject server stopped on {}:{}", g_args->ListenAddress, SnmpRejectServer::PORT);
		}
	}
	catch (const std::exception& e) {
		spdlog::error("Error: {}", e.what());
		return 1;
	}
	return 0;
}

static VOID WINAPI svcMain(DWORD, LPTSTR*) {
	g_serviceStatusHandle = RegisterServiceCtrlHandlerExA(PROJECT_NAME, svcCtrlHandler, nullptr);
	if (g_serviceStatusHandle == nullptr) {
		spdlog::error("Failed to register service control handler: {}", GetLastError());
		return;
	}
	reportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 0, kServiceStatusWaitHintMs);
	reportServiceStatus(SERVICE_RUNNING);
	const auto result = g_args->ServiceOperation.has_value() ? EXIT_FAILURE : mainRun();
	g_servExitCode.store(result);
	if (result == EXIT_SUCCESS) {
		reportServiceStatus(SERVICE_STOPPED);
	}
	else {
		reportServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, static_cast<DWORD>(result));
	}
	g_serviceStatusHandle = nullptr;
}

int main(int argc, char* argv[]) {
	setupLogger(g_args->LogLevel);
	spdlog::debug("Parsed arguments: \n{}", g_args->Dump());
	SERVICE_TABLE_ENTRYA DispatchTable[] = {
		{ const_cast<LPSTR>(PROJECT_NAME), svcMain},
		{ NULL, NULL }
	};
	const auto dispatched = StartServiceCtrlDispatcherA(DispatchTable);
	if (dispatched) {
		return g_servExitCode.load();
	}
	const auto dispatcherError = GetLastError();
	if (dispatcherError == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
		if(g_args->ServiceOperation.has_value()) {
			if(!utility::IsRunAsAdmin()) {
				spdlog::error("This operation requires administrative privileges. Please run as administrator.");
				return 1;
			}
			try {
				serviceActionHandle(g_args->ServiceOperation.value());
				std::cout << "Service operation " << g_args->ServiceOperation.value() << " completed successfully." << std::endl;
			} catch (const std::exception& e) {
				std::cerr << "Service operation failed: " << e.what() << std::endl;
				return 1;
			}
			return 0;
		}
		SetConsoleCtrlHandler(consoleHandler, TRUE);
		return mainRun();
	}
	spdlog::error("Failed to start service control dispatcher: {}", dispatcherError);
	return 1;
}
