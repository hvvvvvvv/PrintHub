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
#include <cstdlib>
#include <functional>
#include <thread>
#include <Arguments.hpp>
#include <NetworkRuntime.h>
#include <SnmpServer.h>
#include <PpmMib.h>
#include <PrinterMetadataCollector.h>

namespace {

constexpr auto kLogRotationHour = 0;
constexpr auto kLogRotationMinute = 0;
constexpr auto kLogRetentionDays = 7;
constexpr auto kLogFlushInterval = std::chrono::seconds(3);
constexpr DWORD kServiceStatusWaitHintMs = 60000;
constexpr DWORD kInitialServiceCheckpoint = 1;
constexpr DWORD kServiceStopCheckpointIntervalMs = 5000;
constexpr DWORD kMetadataDiagnosticPollMs = 1000;
constexpr auto kMetadataShutdownBudget = std::chrono::milliseconds(500);

spdlog::level::level_enum toSpdlogLevel(ServiceLogLevel level) {
	switch (level) {
	case ServiceLogLevel::Trace: return spdlog::level::trace;
	case ServiceLogLevel::Debug: return spdlog::level::debug;
	case ServiceLogLevel::Info: return spdlog::level::info;
	case ServiceLogLevel::Warn: return spdlog::level::warn;
	case ServiceLogLevel::Error: return spdlog::level::err;
	case ServiceLogLevel::Critical: return spdlog::level::critical;
	case ServiceLogLevel::Off: return spdlog::level::off;
	}
	throw std::invalid_argument("Unknown log level");
}

} // namespace

std::atomic<bool> g_shutdown = false;
HANDLE g_shutdownEvent = nullptr;
std::atomic<int> g_servExitCode = EXIT_SUCCESS;
SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
std::atomic<DWORD> g_serviceCheckpoint = kInitialServiceCheckpoint;
std::unique_ptr<const Arguments> g_args;

static void signalShutdown() {
	g_shutdown.store(true);
	if (g_shutdownEvent) {
		SetEvent(g_shutdownEvent);
	}
}

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
		args.arguments = g_args->GetRawArguments();
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
			servers.push_back(std::make_unique<RawPrinting>(printerName, port, g_args->RawServerMinThreads, g_args->MaxPendingClients));
		}
	}
	if (g_args->LpdQueuesConfig.has_value()) {
		servers.push_back(std::make_unique<LPD>(g_args->LpdPort, *g_args->LpdQueuesConfig, g_args->LpdMinThreads,
			g_args->LpdMaxControlFileBytes, g_args->MaxPendingClients));
	}
	return servers;
}

static PpmDirectory createPpmDirectory() {
	ServiceConfiguration configuration;
	configuration.snmpPpm = true;
	configuration.listenAddress = g_args->ListenAddress;
	configuration.lpdPort = g_args->LpdPort;
	if (g_args->RawServersConfig.has_value()) {
		for (const auto& server : *g_args->RawServersConfig) {
			configuration.rawServers.push_back({ std::get<0>(server), std::get<1>(server) });
		}
	}
	if (g_args->LpdQueuesConfig.has_value()) configuration.lpdQueues = *g_args->LpdQueuesConfig;
	return PpmMib::BuildDirectory(configuration);
}

static BOOL WINAPI consoleHandler(DWORD ctrlType) {
	switch (ctrlType) {
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
		case CTRL_CLOSE_EVENT:
		case CTRL_LOGOFF_EVENT:
		case CTRL_SHUTDOWN_EVENT:
			spdlog::info("Received console control signal: {}", ctrlType);
			signalShutdown();
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
			signalShutdown();
			spdlog::info("Service stopping...");
			return NO_ERROR;
		}
		case SERVICE_CONTROL_INTERROGATE: {
			return NO_ERROR;
		}
		default: {
			return ERROR_CALL_NOT_IMPLEMENTED;
		}
	}
}

class ServiceStopProgressReporter final {
public:
    explicit ServiceStopProgressReporter(bool enabled) {
        if (!enabled) return;
        m_doneEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!m_doneEvent) {
            spdlog::warn("Could not create service stop progress event: {}", GetLastError());
            return;
        }
        try {
            m_thread = std::jthread([this](std::stop_token) {
                while (WaitForSingleObject(m_doneEvent, kServiceStopCheckpointIntervalMs) == WAIT_TIMEOUT) {
                    reportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 0, kServiceStatusWaitHintMs);
                }
            });
        } catch (const std::exception& error) {
            CloseHandle(m_doneEvent);
            m_doneEvent = nullptr;
            spdlog::warn("Could not start service stop progress reporter: {}", error.what());
        }
    }

    ~ServiceStopProgressReporter() {
        if (m_doneEvent) {
            SetEvent(m_doneEvent);
            if (m_thread.joinable()) m_thread.join();
            CloseHandle(m_doneEvent);
        }
    }

    ServiceStopProgressReporter(const ServiceStopProgressReporter&) = delete;
    ServiceStopProgressReporter& operator=(const ServiceStopProgressReporter&) = delete;

private:
    HANDLE m_doneEvent = nullptr;
    std::jthread m_thread;
};

struct RunResult {
	int code = EXIT_SUCCESS;
	bool terminateWithoutDllDetach = false;
};

class PrintListenerCleanup {
public:
	explicit PrintListenerCleanup(const std::vector<std::unique_ptr<PrintServer>>& servers) : m_servers(servers) {}
	~PrintListenerCleanup() { Stop(); }
	void Stop() {
		if (m_stopped) return;
		for (const auto& server : m_servers) server->Stop();
		m_stopped = true;
	}
private:
	const std::vector<std::unique_ptr<PrintServer>>& m_servers;
	bool m_stopped = false;
};

static RunResult mainRun(const std::function<void()>& onStarted = {}) {
	spdlog::default_logger()->info("Starting {} version {}", PROJECT_NAME, PROJECT_VERSION);
	spdlog::info("Build date: {}", __DATE__);
	spdlog::info("Build time: {}", __TIME__);

	RunResult result;
	std::shared_ptr<ppm::PrinterMetadataCollector> metadataCollector;
	try {
		WinsockRuntime winsockRuntime;
		auto servers = createRawServers();
		PrintListenerCleanup printCleanup(servers);
		std::optional<SnmpServer> snmpServer;
		bool started = false;
		try {
			for (auto& server : servers) {
				server->SetListenAddr(g_args->ListenAddress);
				server->Start();
				spdlog::info("Server started on {}:{}", server->GetListenAddr(), server->GetListenPort());
			}
			if (g_args->SnmpPpm) {
				auto directory = std::make_shared<const PpmDirectory>(createPpmDirectory());
				std::vector<std::wstring> queueNames;
				for (const auto& printer : directory->printers) queueNames.push_back(printer.queueName);
				metadataCollector = std::make_shared<ppm::PrinterMetadataCollector>(std::move(queueNames));
				snmpServer.emplace(g_args->ListenAddress,
					[directory, metadataCollector](std::string_view request) {
						const auto statuses = metadataCollector->GetStatuses();
						const auto identities = metadataCollector->GetIdentities();
						const auto now = ppm::Clock::now();
						const auto mib = PpmMib::WithMetadata(*directory, *statuses, *identities, now);
						return snmp::Respond(request, mib);
					},
					SnmpServer::PORT, [](int) { signalShutdown(); });
			}
			if (snmpServer) {
				snmpServer->Start();
				spdlog::info("SNMP PPM server started on {}:{}", g_args->ListenAddress, SnmpServer::PORT);
			}
			if (!g_shutdown.load()) {
				if (onStarted) onStarted();
				started = true;
				if (metadataCollector) {
					spdlog::info("PPM discovery does not enable Windows SNMP status polling; check existing client port settings separately");
					metadataCollector->Start();
				}
			}
			while (!g_shutdown.load()) {
				const auto wait = WaitForSingleObject(g_shutdownEvent, metadataCollector ? kMetadataDiagnosticPollMs : INFINITE);
				if (wait == WAIT_OBJECT_0) break;
				if (wait != WAIT_TIMEOUT) {
					spdlog::error("Failed while waiting for shutdown event: {}", GetLastError());
					result.code = EXIT_FAILURE;
					break;
				}
				if (metadataCollector) metadataCollector->LogDiagnostics();
			}
		} catch (const std::exception& error) {
			spdlog::error("Error: {}", error.what());
			result.code = EXIT_FAILURE;
		} catch (...) {
			spdlog::error("Unexpected error in PrintHub service");
			result.code = EXIT_FAILURE;
		}

		if (metadataCollector) metadataCollector->RequestStop();
		ServiceStopProgressReporter stopProgress(started && g_serviceStatusHandle != nullptr);
		if (snmpServer) {
			snmpServer->Stop();
			if (snmpServer->GetFailureCode() != 0) result.code = EXIT_FAILURE;
			spdlog::info("SNMP server stopped on {}:{}", g_args->ListenAddress, SnmpServer::PORT);
		}
		printCleanup.Stop();
	}
	catch (const std::exception& e) {
		spdlog::error("Error: {}", e.what());
		result.code = EXIT_FAILURE;
	}
	if (metadataCollector) {
		result.terminateWithoutDllDetach = !metadataCollector->StopUntil(ppm::Clock::now() + kMetadataShutdownBudget);
		metadataCollector->LogDiagnostics();
		if (result.terminateWithoutDllDetach) {
			metadataCollector->LogUnfinishedWorkers();
			spdlog::warn("PrintHub will exit without DLL detach; printer metadata collection is still blocked");
			spdlog::default_logger()->flush();
		}
	}
	return result;
}

[[noreturn]] static void finishWithoutDllDetach(int exitCode) noexcept {
	// Other components have already stopped. Do not unwind the CRT or run DLL
	// detach with a printer provider still executing on a detached worker.
	TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
	std::abort(); // TerminateProcess on the current process does not return on success.
}

static VOID WINAPI svcMain(DWORD, LPTSTR*) {
	g_serviceStatusHandle = RegisterServiceCtrlHandlerExA(PROJECT_NAME, svcCtrlHandler, nullptr);
	if (g_serviceStatusHandle == nullptr) {
		spdlog::error("Failed to register service control handler: {}", GetLastError());
		return;
	}
	reportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 0, kServiceStatusWaitHintMs);
	const auto result = g_args->ServiceOperation.has_value() ? RunResult{EXIT_FAILURE} : mainRun([] { reportServiceStatus(SERVICE_RUNNING); });
	g_servExitCode.store(result.code);
	if (result.code == EXIT_SUCCESS) {
		reportServiceStatus(SERVICE_STOPPED);
	}
	else {
		reportServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, static_cast<DWORD>(result.code));
	}
	if (result.terminateWithoutDllDetach) finishWithoutDllDetach(result.code);
	g_serviceStatusHandle = nullptr;
}

int main(int argc, char* argv[]) {
	auto parsedArguments = std::make_unique<Arguments>();
	try {
		parsedArguments->Parse(argc, argv);
	} catch (const std::exception& error) {
		std::cerr << "Failed to parse arguments: " << error.what() << std::endl;
		parsedArguments->PrintHelp();
		return EXIT_FAILURE;
	}
	g_args = std::move(parsedArguments);
	g_shutdownEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!g_shutdownEvent) {
		std::cerr << "Failed to create shutdown event: " << GetLastError() << std::endl;
		return 1;
	}
	const auto closeShutdownEvent = []() {
		CloseHandle(g_shutdownEvent);
		g_shutdownEvent = nullptr;
	};
	setupLogger(toSpdlogLevel(g_args->LogLevel));
	spdlog::debug("Parsed arguments: \n{}", g_args->Dump());
	SERVICE_TABLE_ENTRYA DispatchTable[] = {
		{ const_cast<LPSTR>(PROJECT_NAME), svcMain},
		{ NULL, NULL }
	};
	const auto dispatched = StartServiceCtrlDispatcherA(DispatchTable);
	if (dispatched) {
		const auto result = g_servExitCode.load();
		closeShutdownEvent();
		return result;
	}
	const auto dispatcherError = GetLastError();
	if (dispatcherError == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
		if(g_args->ServiceOperation.has_value()) {
			if(!utility::IsRunAsAdmin()) {
				spdlog::error("This operation requires administrative privileges. Please run as administrator.");
				closeShutdownEvent();
				return 1;
			}
			try {
				serviceActionHandle(g_args->ServiceOperation.value());
				std::cout << "Service operation " << g_args->ServiceOperation.value() << " completed successfully." << std::endl;
			} catch (const std::exception& e) {
				std::cerr << "Service operation failed: " << e.what() << std::endl;
				closeShutdownEvent();
				return 1;
			}
			closeShutdownEvent();
			return 0;
		}
		SetConsoleCtrlHandler(consoleHandler, TRUE);
		const auto result = mainRun();
		if (result.terminateWithoutDllDetach) finishWithoutDllDetach(result.code);
		SetConsoleCtrlHandler(consoleHandler, FALSE);
		closeShutdownEvent();
		return result.code;
	}
	spdlog::error("Failed to start service control dispatcher: {}", dispatcherError);
	closeShutdownEvent();
	return 1;
}
