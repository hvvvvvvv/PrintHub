#include <Windows.h>
#include <winspool.h>
#include <winsvc.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <config.h>
#include <filesystem>
#include <minwindef.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/spdlog.h>
#include <utility.h>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <fmt/format.h>
#include <CommandLine.hpp>
#include <ConfigurationFile.hpp>
#include <LogBuffer.h>
#include <PrintRuntime.h>
#include <TextEncoding.h>
#include <WebServer.h>
#include <nlohmann/json.hpp>

extern "C" BOOLEAN NTAPI SystemFunction036(PVOID RandomBuffer, ULONG RandomBufferLength);

namespace {

constexpr auto kLogRotationHour = 0;
constexpr auto kLogRotationMinute = 0;
constexpr auto kLogRetentionDays = 7;
constexpr auto kLogFlushInterval = std::chrono::seconds(3);
constexpr DWORD kServiceStatusWaitHintMs = 60000;
constexpr DWORD kInitialServiceCheckpoint = 1;
constexpr DWORD kServiceStopCheckpointIntervalMs = 5000;
constexpr DWORD kMetadataDiagnosticPollMs = 1000;

std::shared_ptr<LogBuffer> g_logBuffer;

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

namespace {

using Json = nlohmann::json;

struct WebSettings {
	std::string listenAddress;
	std::uint16_t port;
	std::string accessToken;
};

std::string GenerateAccessToken() {
	std::array<unsigned char, 32> bytes{};
	if (!SystemFunction036(bytes.data(), static_cast<ULONG>(bytes.size()))) {
		throw std::runtime_error("Could not generate web access token");
	}
	constexpr char hex[] = "0123456789abcdef";
	std::string token(bytes.size() * 2, '\0');
	for (std::size_t index = 0; index < bytes.size(); ++index) {
		token[index * 2] = hex[bytes[index] >> 4];
		token[index * 2 + 1] = hex[bytes[index] & 0x0f];
	}
	return token;
}

WebSettings ResolveWebSettings(const CommandLine& commandLine) {
	return {
		commandLine.webBind.value_or("127.0.0.1"),
		commandLine.webPort.value_or(8323),
		commandLine.webToken ? *commandLine.webToken : GenerateAccessToken()
	};
}

void LogWebAccessToken(const std::string& token, bool generated) {
	const auto logger = spdlog::default_logger();
	const auto previous = logger->level();
	if (previous > spdlog::level::info) logger->set_level(spdlog::level::info);
	try {
		if (generated) logger->info("Web UI access token generated for this start: {}", token);
		else logger->info("Web UI access token: {}", token);
	} catch (...) {
		if (previous > spdlog::level::info) logger->set_level(previous);
		throw;
	}
	if (previous > spdlog::level::info) logger->set_level(previous);
	logger->flush();
}

struct HandleCloser {
	void operator()(HANDLE handle) const noexcept {
		if (handle) CloseHandle(handle);
	}
};

// Stop(false) retains WebServer callbacks until process exit. Everything they
// use must therefore be owned by those callbacks, rather than mainRun's stack.
struct WebRuntimeState {
	PrintRuntime runtime;
	std::atomic<std::uint16_t> webPort{0};
	std::unique_ptr<void, HandleCloser> runtimeChangedEvent;
	std::mutex applyMutex;
	std::atomic<bool> stopping{false};
	std::filesystem::path configPath;
	std::chrono::system_clock::time_point startedAt;
	std::chrono::steady_clock::time_point startedSteady;
	bool serviceMode = false;
};

std::string DumpJson(const Json& value) {
	return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string RecentLogsJson(const std::shared_ptr<LogBuffer>& buffer, int tail) {
	Json lines = Json::array();
	if (buffer) {
		const auto count = static_cast<std::size_t>((std::max)(tail, 0));
		for (const auto& line : buffer->Snapshot(count)) lines.push_back(line);
	}
	return DumpJson({{"lines", std::move(lines)}});
}

std::string InstalledPrinterNamesJson() {
	constexpr DWORD kMaxPrinterBufferBytes = 8 * 1024 * 1024;
	std::vector<BYTE> buffer;
	DWORD bytesNeeded = 0;
	DWORD printerCount = 0;
	// Probe with a null buffer, then fill it with at most two resize retries.
	for (int attempt = 0; ; ++attempt) {
		if (EnumPrintersW(PRINTER_ENUM_LOCAL, nullptr, 2,
			buffer.empty() ? nullptr : buffer.data(), static_cast<DWORD>(buffer.size()),
			&bytesNeeded, &printerCount)) break;
		const auto error = GetLastError();
		if (error == ERROR_INSUFFICIENT_BUFFER && bytesNeeded > kMaxPrinterBufferBytes) {
			throw std::runtime_error("Installed printer list is too large");
		}
		if (error != ERROR_INSUFFICIENT_BUFFER || attempt == 3) {
			throw std::runtime_error("Cannot enumerate installed printers: Win32 error " + std::to_string(error));
		}
		buffer.resize(bytesNeeded);
	}

	std::vector<std::wstring> wideNames;
	const auto* printers = reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data());
	for (DWORD index = 0; index < printerCount; ++index) {
		const auto& printer = printers[index];
		if (!printer.pPrinterName || (printer.Attributes & PRINTER_ATTRIBUTE_NETWORK)) continue;
		wideNames.emplace_back(printer.pPrinterName);
	}
	std::sort(wideNames.begin(), wideNames.end(), [](const std::wstring& left, const std::wstring& right) {
		return _wcsicmp(left.c_str(), right.c_str()) < 0;
	});
	wideNames.erase(std::unique(wideNames.begin(), wideNames.end(),
		[](const std::wstring& left, const std::wstring& right) {
			return _wcsicmp(left.c_str(), right.c_str()) == 0;
		}), wideNames.end());
	std::vector<std::string> names;
	names.reserve(wideNames.size());
	for (const auto& name : wideNames) names.push_back(WideToUtf8(name));
	return DumpJson({{"printers", names}});
}

Json UtcTimestamp(std::int64_t unixSeconds) {
	const auto time = static_cast<std::time_t>(unixSeconds);
	std::tm utc{};
	if (gmtime_s(&utc, &time) != 0) return nullptr;
	char formatted[32]{};
	if (std::strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) return nullptr;
	return std::string(formatted);
}

Json QueryServiceState() {
	const auto manager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!manager) return nullptr;
	const auto service = OpenServiceA(manager, PROJECT_NAME, SERVICE_QUERY_STATUS);
	if (!service) {
		const auto error = GetLastError();
		CloseServiceHandle(manager);
		if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
			return {{"installed", false}, {"running", false}};
		}
		return nullptr;
	}
	SERVICE_STATUS status{};
	const auto queried = QueryServiceStatus(service, &status);
	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	if (!queried) return nullptr;
	return {{"installed", true}, {"running", status.dwCurrentState == SERVICE_RUNNING}};
}

const char* ApplyStateName(PrintRuntime::ApplyState state) {
	switch (state) {
	case PrintRuntime::ApplyState::Idle: return "idle";
	case PrintRuntime::ApplyState::Applying: return "applying";
	case PrintRuntime::ApplyState::Error: return "error";
	}
	return "error";
}

std::string StatusJson(const WebRuntimeState& state, const WebSettings& webSettings) {
	const auto snapshot = state.runtime.GetSnapshot();
	Json rawServers = Json::array();
	for (const auto& listener : snapshot.rawListeners) {
		rawServers.push_back({
			{"printerName", AcpToUtf8(listener.printerName)},
			{"listenAddress", listener.listenAddress},
			{"port", listener.port}
		});
	}
	const auto startedUnixSeconds = std::chrono::duration_cast<std::chrono::seconds>(
		state.startedAt.time_since_epoch()).count();
	const auto uptimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - state.startedSteady).count();
	return DumpJson({
		{"version", PROJECT_VERSION},
		{"configPath", WideToUtf8(state.configPath.native())},
		{"mode", state.serviceMode ? "service" : "console"},
		{"startedAt", UtcTimestamp(startedUnixSeconds)},
		{"uptimeSeconds", uptimeSeconds},
		{"service", QueryServiceState()},
		{"web", {{"listenAddress", webSettings.listenAddress}, {"port", state.webPort.load()}}},
		{"lpd", {{"listenAddress", snapshot.lpdListenAddress},
			{"port", snapshot.lpdPort}, {"running", snapshot.lpdRunning}}},
		{"rawServers", std::move(rawServers)},
		{"mappingCount", snapshot.mappingCount},
		{"ppm", {{"enabled", snapshot.ppmEnabled}, {"running", snapshot.ppmRunning}}},
		{"apply", {{"state", ApplyStateName(snapshot.applyState)},
			{"lastError", snapshot.lastError.empty() ? Json(nullptr) : Json(snapshot.lastError)},
			{"lastAppliedAt", snapshot.lastAppliedUnixSeconds == 0
				? Json(nullptr) : UtcTimestamp(snapshot.lastAppliedUnixSeconds)}}}
	});
}

std::string CurrentExceptionMessage() {
	try {
		throw;
	} catch (const std::exception& error) {
		return error.what();
	} catch (...) {
		return "unexpected error";
	}
}

printhub::web::ApplyResponse ApplyConfiguration(WebRuntimeState& state, std::string_view body) {
	// Apply and final shutdown take this mutex before entering PrintRuntime.
	// Status only tries the runtime mutex and never takes the transaction lock.
	std::unique_lock lock(state.applyMutex, std::try_to_lock);
	if (!lock.owns_lock()) {
		return {409, DumpJson({{"error", "configuration apply already in progress"}})};
	}
	if (state.stopping.load()) {
		return {500, DumpJson({{"error", "Print runtime is shutting down"}})};
	}
	auto& runtime = state.runtime;
	const auto& configPath = state.configPath;
	ServiceConfiguration configuration;
	std::string nextJson;
	try {
		configuration = ParseConfigurationJson(body, "web request");
		nextJson = SerializeConfigurationJson(configuration);
	} catch (const std::exception& error) {
		return {400, DumpJson({{"error", error.what()}})};
	}
	std::string previousJson;
	try {
		previousJson = runtime.CurrentConfigJson();
		const bool runtimeRunning = runtime.GetSnapshot().running;
		if (runtimeRunning && nextJson == previousJson) {
			bool fileMatches = false;
			try {
				fileMatches = SerializeConfigurationJson(LoadConfigurationFile(configPath)) == previousJson;
			} catch (const std::exception&) {
				// A missing, unreadable or invalid file also needs repair.
			}
			if (!fileMatches) {
				SaveConfigurationFile(configPath, configuration);
				return {200, DumpJson({{"applied", false}, {"unchanged", true}, {"fileRepaired", true}})};
			}
			return {200, DumpJson({{"applied", false}, {"unchanged", true}})};
		}
		SaveConfigurationFile(configPath, configuration);
	} catch (const std::exception& error) {
		return {500, DumpJson({{"error", error.what()}})};
	}

	try {
		runtime.Apply(configuration);
	} catch (...) {
		auto error = CurrentExceptionMessage();
		bool rolledBack = false;
		try {
			const auto restoredConfiguration = runtime.CopyConfiguration();
			const bool runtimeRestored = runtime.GetSnapshot().running &&
				SerializeConfigurationJson(restoredConfiguration) == previousJson;
			if (runtimeRestored) {
				try {
					SaveConfigurationFile(configPath, restoredConfiguration);
					rolledBack = true;
				} catch (...) {
					error += " (runtime restored, but configuration file rollback failed: " + CurrentExceptionMessage() + ")";
				}
			}
		} catch (...) {
			error += " (could not verify rollback: " + CurrentExceptionMessage() + ")";
		}
		SetEvent(state.runtimeChangedEvent.get());
		spdlog::error("Web configuration apply failed: {}", error);
		return {500, DumpJson({{"error", error}, {"rolledBack", rolledBack}})};
	}
	SetEvent(state.runtimeChangedEvent.get());
	return {200, DumpJson({{"applied", true}, {"unchanged", false}})};
}

} // namespace

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
	g_logBuffer = std::make_shared<LogBuffer>(80);
	spdlog::default_logger()->sinks().push_back(g_logBuffer);

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
		std::error_code existsError;
		if (std::filesystem::exists(g_commandLine->configPath, existsError) || existsError) {
			(void)LoadConfigurationFile(g_commandLine->configPath);
		}
		utility::ServiceManager::InstallArgs args;
		args.binaryPath = utility::getExecutablePathW();
		args.arguments = BuildInstallArguments(*g_commandLine);
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

static RunResult mainRun(const std::function<void()>& onStarted = {}) {
	std::string displayPath;
	try {
		displayPath = WideToUtf8(g_commandLine->configPath.native());
		bool created = false;
		g_configuration = LoadOrCreateConfigurationFile(g_commandLine->configPath, &created);
		if (created) {
			spdlog::info("Configuration file '{}' was not found; created default configuration with an empty printerMap", displayPath);
		}
	} catch (const std::exception& error) {
		spdlog::error("Failed to load configuration '{}': {}", displayPath, error.what());
		return { EXIT_FAILURE, false };
	}
	spdlog::set_level(toSpdlogLevel(g_configuration->logLevel));
	spdlog::info("Starting {} version {} with configuration '{}'", PROJECT_NAME, PROJECT_VERSION, displayPath);
	spdlog::info("Build date: {}", __DATE__);
	spdlog::info("Build time: {}", __TIME__);

	RunResult result;
	const auto state = std::make_shared<WebRuntimeState>();
	state->configPath = g_commandLine->configPath;
	state->serviceMode = g_serviceStatusHandle != nullptr;
	auto& runtime = state->runtime;
	auto& webPort = state->webPort;
	auto& runtimeChangedEvent = state->runtimeChangedEvent;
	std::unique_ptr<printhub::web::Server> web;
	bool started = false;
	try {
		runtime.Start(*g_configuration);
		state->startedAt = std::chrono::system_clock::now();
		state->startedSteady = std::chrono::steady_clock::now();
		try {
			const auto settings = ResolveWebSettings(*g_commandLine);
			LogWebAccessToken(settings.accessToken, !g_commandLine->webToken.has_value());
			webPort.store(settings.port);
			// A hot apply can create the first metadata collector while the main
			// loop is waiting indefinitely. Wake it without signalling shutdown.
			runtimeChangedEvent.reset(CreateEventA(nullptr, FALSE, FALSE, nullptr));
			if (!runtimeChangedEvent) {
				throw std::runtime_error("Could not create configuration change event (Win32 error " +
					std::to_string(GetLastError()) + ")");
			}
			printhub::web::Options options;
			options.listenAddress = settings.listenAddress;
			options.port = settings.port;
			options.accessToken = settings.accessToken;
			options.version = PROJECT_VERSION;
			options.statusJson = [state, settings] {
				if (state->stopping.load()) throw std::runtime_error("Print runtime is shutting down");
				return StatusJson(*state, settings);
			};
			options.configJson = [state] {
				if (state->stopping.load()) throw std::runtime_error("Print runtime is shutting down");
				return state->runtime.CurrentConfigJson();
			};
			options.logsJson = [buffer = g_logBuffer](int tail) { return RecentLogsJson(buffer, tail); };
			options.printersJson = [] { return InstalledPrinterNamesJson(); };
			options.applyConfig = [state](std::string_view body) {
				return ApplyConfiguration(*state, body);
			};
			web = std::make_unique<printhub::web::Server>(std::move(options));
			webPort.store(web->Start());
			spdlog::info("Web UI started on {}:{}", settings.listenAddress, webPort.load());
		} catch (const std::exception& error) {
			web.reset();
			spdlog::error("Web UI disabled: {}", error.what());
		} catch (...) {
			web.reset();
			spdlog::error("Web UI disabled: unexpected error");
		}
		if (!g_shutdown.load()) {
			if (onStarted) onStarted();
			started = true;
		}
		while (!g_shutdown.load()) {
			const auto timeout = runtime.HasActiveMetadata() ? kMetadataDiagnosticPollMs : INFINITE;
			const HANDLE events[]{g_shutdownEvent, runtimeChangedEvent.get()};
			const auto wait = runtimeChangedEvent
				? WaitForMultipleObjects(2, events, FALSE, timeout)
				: WaitForSingleObject(g_shutdownEvent, timeout);
			if (wait == WAIT_OBJECT_0) break;
			if (runtimeChangedEvent && wait == WAIT_OBJECT_0 + 1) continue;
			if (wait != WAIT_TIMEOUT) {
				spdlog::error("Failed while waiting for shutdown event: {}", GetLastError());
				result.code = EXIT_FAILURE;
				break;
			}
			runtime.PollDiagnostics();
		}
	} catch (const std::exception& error) {
		spdlog::error("Error: {}", error.what());
		result.code = EXIT_FAILURE;
	} catch (...) {
		spdlog::error("Unexpected error in PrintHub service");
		result.code = EXIT_FAILURE;
	}
	ServiceStopProgressReporter stopProgress(started && g_serviceStatusHandle != nullptr);
	state->stopping.store(true);
	if (web) web->Stop(false); // Process exit follows; slow HTTP clients must not delay shutdown.
	{
		// Finish an already-running disk/apply transaction before stopping the
		// runtime. Requests still receiving a body will be rejected on dispatch.
		std::lock_guard lock(state->applyMutex);
		runtime.Stop();
		result.terminateWithoutDllDetach |= runtime.NeedsDetachBypass();
	}
	if (result.terminateWithoutDllDetach) {
		spdlog::warn("PrintHub will exit without DLL detach; printer metadata collection is still blocked");
		spdlog::default_logger()->flush();
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
