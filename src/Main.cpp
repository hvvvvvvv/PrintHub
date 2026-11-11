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
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <fmt/format.h>
#include <CommandLine.hpp>
#include <ConfigurationFile.hpp>
#include <NetworkRuntime.h>
#include <SnmpServer.h>
#include <PpmMib.h>
#include <PrinterMetadataCollector.h>
#include <TextEncoding.h>

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

std::wstring widenAscii(std::string_view value) {
	return std::wstring(value.begin(), value.end());
}

std::string logPathToAcp(const std::filesystem::path& logPath) {
	try {
		return WideToAcp(logPath.native());
	} catch (const std::invalid_argument&) {
		// The existing directory may have an ANSI-representable 8.3 short name.
	}
	const auto logDirectory = logPath.parent_path();
	const auto count = GetShortPathNameW(logDirectory.c_str(), nullptr, 0);
	if (count != 0) {
		std::wstring shortDirectory(static_cast<size_t>(count), L'\0');
		const auto length = GetShortPathNameW(logDirectory.c_str(), shortDirectory.data(), count);
		if (length != 0 && length < count) {
			shortDirectory.resize(static_cast<size_t>(length));
			try {
				return WideToAcp((std::filesystem::path(shortDirectory) / logPath.filename()).native());
			} catch (const std::invalid_argument&) {
				// Short names can be unavailable or still outside the system code page.
			}
		}
	}
	throw std::runtime_error("Log path cannot be represented in the system ANSI code page");
}

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
std::unique_ptr<CommandLine> g_commandLine;
std::optional<ServiceConfiguration> g_configuration;

static void signalShutdown() {
	g_shutdown.store(true);
	if (g_shutdownEvent) {
		SetEvent(g_shutdownEvent);
	}
}

static void setupLogger() {
	const auto logDir = utility::getExecutablePathW().parent_path() / L"logs";
	std::filesystem::create_directories(logDir);
	const auto logPath = logDir / (widenAscii(PROJECT_NAME) + L".log");
	auto fileSink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(logPathToAcp(logPath), kLogRotationHour, kLogRotationMinute, false, kLogRetentionDays);
	fileSink->set_level(spdlog::level::trace);
	spdlog::default_logger()->sinks().push_back(fileSink);

	spdlog::set_pattern("%Y-%m-%d %H:%M:%S.%e %l : %v");

	spdlog::set_level(spdlog::level::info);

	spdlog::flush_every(kLogFlushInterval);
	
}

static void serviceActionHandle(ServiceAction action) {
	auto serviceManager = utility::ServiceManager(PROJECT_NAME);
	switch (action) {
	case ServiceAction::Install: {
		if (serviceManager.IsInstalled()) {
			throw std::runtime_error(fmt::format("Service {} is already installed.", PROJECT_NAME));
		}
		(void)LoadConfigurationFile(g_commandLine->configPath);
		utility::ServiceManager::InstallArgs args;
		args.binaryPath = utility::getExecutablePathW();
		args.arguments = { L"--config", g_commandLine->configPath.native() };
		args.dependencies = { L"Tcpip" };
		args.displayName = widenAscii(PROJECT_NAME);
		serviceManager.InstallSrv(args);
		break;
	}
	case ServiceAction::Uninstall:
		if (!serviceManager.IsInstalled()) {
			throw std::runtime_error(fmt::format("Service {} is not installed.", PROJECT_NAME));
		}
		serviceManager.UninstallSrv();
		break;
	case ServiceAction::Start:
		if (!serviceManager.IsInstalled()) {
			throw std::runtime_error(fmt::format("Service {} is not installed.", PROJECT_NAME));
		}
		if (serviceManager.IsRunning()) {
			throw std::runtime_error(fmt::format("Service {} is already running.", PROJECT_NAME));
		}
		serviceManager.StartSrv();
		break;
	case ServiceAction::Stop:
		if (!serviceManager.IsInstalled()) {
			throw std::runtime_error(fmt::format("Service {} is not installed.", PROJECT_NAME));
		}
		if (!serviceManager.IsRunning()) {
			throw std::runtime_error(fmt::format("Service {} is not running.", PROJECT_NAME));
		}
		serviceManager.StopSrv();
		break;
	case ServiceAction::Restart:
		if (!serviceManager.IsInstalled()) {
			throw std::runtime_error(fmt::format("Service {} is not installed.", PROJECT_NAME));
		}
		serviceManager.RestartSrv();
		break;
	default:
		throw std::invalid_argument("Unknown service action");
	}
}

static std::vector<std::unique_ptr<PrintServer>> createPrintServers(const ServiceConfiguration& configuration) {
	std::vector<std::unique_ptr<PrintServer>> servers;
	if (configuration.printerMap.empty()) {
		spdlog::info("No printers are configured; no print listeners will be started");
		return servers;
	}

	std::vector<LpdQueueMapping> queues;
	queues.reserve(configuration.printerMap.size());
	for (const auto& mapping : configuration.printerMap) {
		if (mapping.rawServer) {
			auto server = std::make_unique<RawPrinting>(mapping.name, mapping.rawServer->port,
				configuration.rawServerMinThreads, configuration.maxPendingClients);
			server->SetListenAddr(mapping.rawServer->listenAddress);
			servers.push_back(std::move(server));
		}
		queues.push_back({ mapping.lprQueue, mapping.name });
	}
	auto lpdServer = std::make_unique<LPD>(configuration.lpdServer.port, std::move(queues), configuration.lpdMinThreads,
		ServiceConfiguration::kDefaultLpdMaxControlFileBytes, configuration.maxPendingClients);
	lpdServer->SetListenAddr(configuration.lpdServer.listenAddress);
	servers.push_back(std::move(lpdServer));
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
	std::string displayPath;
	try {
		displayPath = WideToUtf8(g_commandLine->configPath.native());
		g_configuration = LoadConfigurationFile(g_commandLine->configPath);
	} catch (const std::exception& error) {
		spdlog::error("Failed to load configuration '{}': {}", displayPath, error.what());
		return { EXIT_FAILURE, false };
	}
	spdlog::set_level(toSpdlogLevel(g_configuration->logLevel));
	spdlog::info("Starting {} version {} with configuration '{}'", PROJECT_NAME, PROJECT_VERSION, displayPath);
	spdlog::info("Build date: {}", __DATE__);
	spdlog::info("Build time: {}", __TIME__);

	RunResult result;
	std::shared_ptr<ppm::PrinterMetadataCollector> metadataCollector;
	try {
		WinsockRuntime winsockRuntime;
		auto servers = createPrintServers(*g_configuration);
		PrintListenerCleanup printCleanup(servers);
		std::optional<SnmpServer> snmpServer;
		bool started = false;
		try {
			for (auto& server : servers) {
				server->Start();
				spdlog::info("Server started on {}:{}", server->GetListenAddr(), server->GetListenPort());
			}
			if (!g_configuration->ppmDisabled && !g_configuration->printerMap.empty()) {
				try {
					auto directory = std::make_shared<const PpmDirectory>(PpmMib::BuildDirectory(*g_configuration));
					std::vector<std::wstring> queueNames;
					for (const auto& printer : directory->printers) queueNames.push_back(printer.queueName);
					metadataCollector = std::make_shared<ppm::PrinterMetadataCollector>(std::move(queueNames));
					snmpServer.emplace(g_configuration->lpdServer.listenAddress,
						[directory, metadataCollector](std::string_view request) {
							const auto statuses = metadataCollector->GetStatuses();
							const auto identities = metadataCollector->GetIdentities();
							const auto now = ppm::Clock::now();
							const auto mib = PpmMib::WithMetadata(*directory, *statuses, *identities, now);
							return snmp::Respond(request, mib);
						},
						SnmpServer::PORT, [](int error) {
							spdlog::error("PPM discovery disabled because SNMP failed with WSA error {}", error);
						});
					snmpServer->Start();
					spdlog::info("SNMP PPM server started on {}:{}", g_configuration->lpdServer.listenAddress, SnmpServer::PORT);
				} catch (const std::exception& error) {
					snmpServer.reset();
					metadataCollector.reset();
					spdlog::error("PPM discovery disabled because SNMP could not start: {}", error.what());
				} catch (...) {
					snmpServer.reset();
					metadataCollector.reset();
					spdlog::error("PPM discovery disabled because SNMP could not start: unexpected error");
				}
			} else if (g_configuration->ppmDisabled) {
				spdlog::info("PPM discovery is disabled by configuration");
			} else {
				spdlog::info("PPM discovery skipped because no printers are configured");
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
			spdlog::info("SNMP server stopped on {}:{}", g_configuration->lpdServer.listenAddress, SnmpServer::PORT);
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
		g_servExitCode.store(EXIT_FAILURE);
		return;
	}
	reportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 0, kServiceStatusWaitHintMs);
	const auto result = g_commandLine->serviceAction.has_value() ? RunResult{EXIT_FAILURE} : mainRun([] { reportServiceStatus(SERVICE_RUNNING); });
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

int wmain(int argc, wchar_t* argv[]) {
	try {
		std::vector<std::wstring> arguments;
		for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
		g_commandLine = std::make_unique<CommandLine>(ParseCommandLine(arguments, utility::getExecutablePathW()));
	} catch (const std::exception& error) {
		std::cerr << error.what() << "\n\n";
		PrintCommandLineUsage(std::cerr);
		return EXIT_FAILURE;
	}
	if (g_commandLine->help) {
		PrintCommandLineUsage(std::cout);
		return EXIT_SUCCESS;
	}
	if (g_commandLine->version) {
		std::cout << PROJECT_VERSION << '\n';
		return EXIT_SUCCESS;
	}
	g_shutdownEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!g_shutdownEvent) {
		std::cerr << "Failed to create shutdown event: " << GetLastError() << std::endl;
		return 1;
	}
	const auto closeShutdownEvent = []() {
		CloseHandle(g_shutdownEvent);
		g_shutdownEvent = nullptr;
	};
	try {
		setupLogger();
	} catch (const std::exception& error) {
		std::cerr << "Failed to initialize logging: " << error.what() << std::endl;
		closeShutdownEvent();
		return 1;
	}
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
		if (g_commandLine->serviceAction.has_value()) {
			if(!utility::IsRunAsAdmin()) {
				spdlog::error("This operation requires administrative privileges. Please run as administrator.");
				closeShutdownEvent();
				return 1;
			}
			try {
				serviceActionHandle(*g_commandLine->serviceAction);
				std::cout << "Service operation " << ServiceActionName(*g_commandLine->serviceAction) << " completed successfully." << std::endl;
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
