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


std::atomic<bool> g_shutdown = false;
std::atomic<int> g_servExitCode = EXIT_SUCCESS;
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
	auto fileSink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(logDir.append(logFile).string(), 0, 0, false, 7);
	fileSink->set_level(spdlog::level::trace);
	spdlog::default_logger()->sinks().push_back(fileSink);

	spdlog::set_pattern("%Y-%m-%d %H:%M:%S.%e %l : %v");

	spdlog::set_level(level);

	spdlog::flush_every(std::chrono::seconds(3));
	
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

static DWORD WINAPI svcCtrlHandler(DWORD dwControl, DWORD, LPVOID, LPVOID) {
	switch (dwControl) {
		case SERVICE_CONTROL_STOP:
		case SERVICE_CONTROL_SHUTDOWN: {
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
		auto servers = createRawServers();
		if (servers.empty()) {
			spdlog::warn("No servers configured, exiting.");
			return 1;
		}
		for (auto& server : servers) {
			server->Start();
			spdlog::info("Server started on {}:{}", server->GetListenAddr(), server->GetListenPort());
		}

		while (!g_shutdown.load()) {
			g_shutdown.wait(false);
		}
		for (auto& server : servers) {
			server->Stop();
			spdlog::info("server stopped on {}:{}", server->GetListenAddr(), server->GetListenPort());
		}
	}
	catch (const std::exception& e) {
		spdlog::error("Error: {}", e.what());
		return 1;
	}
	return 0;
}

static VOID WINAPI svcMain(DWORD dwArgc, LPTSTR* lpszArgv) {
	auto statusHandle = RegisterServiceCtrlHandlerEx(const_cast<LPSTR>(""), svcCtrlHandler, NULL);
	if (statusHandle == NULL) {
		spdlog::error("Failed to register service control handler: {}", GetLastError());
		return;
	}
	LPSERVICE_STATUS currentStatus = new SERVICE_STATUS();
	currentStatus->dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	currentStatus->dwWin32ExitCode = NO_ERROR;
	currentStatus->dwServiceSpecificExitCode = NO_ERROR;
	currentStatus->dwWaitHint = 0;
	currentStatus->dwCurrentState = SERVICE_RUNNING;
	currentStatus->dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
	SetServiceStatus(statusHandle, currentStatus);

	if (g_args->ServiceOperation.has_value() || mainRun()) {
		currentStatus->dwCurrentState = SERVICE_STOPPED;
		currentStatus->dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
		currentStatus->dwServiceSpecificExitCode = 1; // Custom error code
		SetServiceStatus(statusHandle, currentStatus);
		g_servExitCode.store(currentStatus->dwWin32ExitCode);
		delete currentStatus;
		return;
	}

	currentStatus->dwControlsAccepted = NULL;
	currentStatus->dwCurrentState = SERVICE_STOPPED;
	currentStatus->dwWin32ExitCode = NO_ERROR;
	SetServiceStatus(statusHandle, currentStatus);
	delete currentStatus;
}

int main(int argc, char* argv[]) {
	setupLogger(g_args->LogLevel);
	spdlog::debug("Parsed arguments: \n{}", g_args->Dump());
	SERVICE_TABLE_ENTRY DispatchTable[] = {
		{ const_cast<LPSTR>(""), (LPSERVICE_MAIN_FUNCTION)svcMain},
		{ NULL, NULL }
	};
	if(!StartServiceCtrlDispatcher(DispatchTable) && GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
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
	} else if(GetLastError() == NOERROR) {
		return g_servExitCode.load();
	} else {
		spdlog::error("Failed to start service control dispatcher: {}", GetLastError());
		return 1;
	}
}
