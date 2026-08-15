#pragma once
#include <string>
#include <vector>
#include <argparse.h>
#include <spdlog/spdlog.h>
#include <optional>
#include <config.h>
#include <utility.h>
#include <tuple>
#include <set>


class Arguments {
	public:
		using RawServersConfigType = std::vector<std::tuple<std::string, uint16_t>>;
		static constexpr auto ARGS_RAW_SERVERS = "--raw-server";
		static constexpr auto ARGS_RAW_SERVER_MIN_THREADS = "--raw-server-min-threads";
		static constexpr auto ARGS_LPD_PORT = "--lpd-port";
		static constexpr auto ARGS_LPD_MIN_THREADS = "--lpd-min-threads";
		static constexpr auto ARGS_LPD_DISABLE = "--lpd-disable";
		static constexpr auto ARGS_LOG_LEVEL = "--log-level";
		static constexpr auto ARGS_SERVICE = "--service";
		static constexpr auto ARGS_SILENT = "--silent";
		static constexpr auto ARGS_SERVICE_INSTALL = "install";
		static constexpr auto ARGS_SERVICE_UNINSTALL = "uninstall";
		static constexpr auto ARGS_SERVICE_START = "start";
		static constexpr auto ARGS_SERVICE_STOP = "stop";
		static constexpr auto ARGS_SERVICE_RESTART = "restart";

		static constexpr uint16_t DEFALUT_MIN_THREADS = 1;
		static constexpr uint16_t DEFAULT_LPD_PORT = 515;
		static constexpr uint16_t DEFAULT_RAW_PORT = 9100;
		static constexpr auto DEFALUT_LOG_LEVEL = spdlog::level::info;
		static constexpr auto DEFALUT_LPD_DISABLE = false;
		static constexpr auto DEFALUT_SILENT = false;

		Arguments(const Arguments&) = delete;
		Arguments& operator=(const Arguments&) = delete;
		Arguments();
		void Parse(int argc, char** argv);
		std::vector<std::string> GetRawArguments(std::optional<std::set<std::string>> ignoreItems) const;
		void PrintHelp() const;
		std::string Dump() const;

		std::optional<RawServersConfigType> RawServersConfig;
		int RawServerMinThreads;
		uint16_t LpdPort;
		bool LpdDisable;
		int LpdMinThreads;
		spdlog::level::level_enum LogLevel;
		std::optional<std::string> ServiceOperation;
		bool Silent;
	private:
		argparse::ArgumentParser m_parser;
		void parseRawServers(const std::vector<std::string>& rawServers);

		static spdlog::level::level_enum parseLogLevel(const std::string& str);		
		static std::string logLevelToString(spdlog::level::level_enum level);
};

Arguments::Arguments() : m_parser(argparse::ArgumentParser(PROJECT_NAME, PROJECT_VERSION)){
	m_parser.add_argument(ARGS_RAW_SERVERS)
		.help("List of raw server printer name port maps")
		.append();
	m_parser.add_argument(ARGS_RAW_SERVER_MIN_THREADS)
		.help("Minimum threads for raw printing server")
		.default_value(DEFALUT_MIN_THREADS)
		.scan<'d', uint16_t>();
	m_parser.add_argument(ARGS_LPD_PORT)
		.help("Port for LPD server")
		.default_value(DEFAULT_LPD_PORT)
		.scan<'d', uint16_t>();
	m_parser.add_argument(ARGS_LPD_MIN_THREADS)
		.help("Minimum threads for LPD server")
		.default_value(DEFALUT_MIN_THREADS)
		.scan<'d', uint16_t>();
	m_parser.add_argument(ARGS_LPD_DISABLE)
		.help("Disable LPD server")
		.default_value(DEFALUT_LPD_DISABLE)
		.implicit_value(true);
	m_parser.add_argument(ARGS_LOG_LEVEL)
		.help("Set log level, choices(trace | debug | info | warn | error | critical)")
		.default_value(spdlog::level::info)
		.action(parseLogLevel);
	m_parser.add_argument(ARGS_SERVICE)
		.help(fmt::format("Service management, choices({} | {} | {} | {} | {}", ARGS_SERVICE_INSTALL, ARGS_SERVICE_UNINSTALL, ARGS_SERVICE_START, ARGS_SERVICE_STOP, ARGS_SERVICE_RESTART))
		.choices(ARGS_SERVICE_INSTALL, ARGS_SERVICE_UNINSTALL, ARGS_SERVICE_START, ARGS_SERVICE_STOP, ARGS_SERVICE_RESTART);
	m_parser.add_argument(ARGS_SILENT)
		.default_value(DEFALUT_SILENT)
		.implicit_value(true)
		.hidden();
	RawServersConfig = std::nullopt;
	RawServerMinThreads = DEFALUT_MIN_THREADS;
	LpdPort = DEFAULT_LPD_PORT;
	LpdDisable = DEFALUT_LPD_DISABLE;
	LpdMinThreads = DEFALUT_MIN_THREADS;
	LogLevel = DEFALUT_LOG_LEVEL;
	ServiceOperation = std::nullopt;
	Silent = DEFALUT_SILENT;
}

void Arguments::Parse(int argc, char** argv) {
	m_parser.parse_args(argc, argv);
	RawServerMinThreads = m_parser.get<uint16_t>(ARGS_RAW_SERVER_MIN_THREADS);
	LpdPort = m_parser.get<uint16_t>(ARGS_LPD_PORT);
	LpdDisable = m_parser.get<bool>(ARGS_LPD_DISABLE);
	LpdMinThreads = m_parser.get<uint16_t>(ARGS_LPD_MIN_THREADS);
	LogLevel = m_parser.get<spdlog::level::level_enum>(ARGS_LOG_LEVEL);
	ServiceOperation = m_parser.is_used(ARGS_SERVICE) ? std::make_optional(m_parser.get<std::string>(ARGS_SERVICE)) : std::nullopt;
	Silent = m_parser.get<bool>(ARGS_SILENT);

	if( m_parser.is_used(ARGS_RAW_SERVERS) ) {
		auto rawServers = m_parser.get<std::vector<std::string>>(ARGS_RAW_SERVERS);
		if (rawServers.empty()) {
			RawServersConfig.reset();
		} else {
			parseRawServers(rawServers);
		}
	} else {
		RawServersConfig.reset();
	}
}

std::vector<std::string> Arguments::GetRawArguments(std::optional<std::set<std::string>> ignoreItems) const {
	std::vector<std::string> result;
	if (RawServersConfig.has_value() && (!ignoreItems.has_value() || !ignoreItems.value().contains(ARGS_RAW_SERVERS))) {
		for (const auto& server : *RawServersConfig) {
			result.push_back(ARGS_RAW_SERVERS);
			result.push_back(fmt::format("{}:{}", std::get<0>(server), std::to_string(std::get<1>(server))));
		}
	}
	if (RawServerMinThreads != DEFALUT_MIN_THREADS && (!ignoreItems.has_value() || !ignoreItems.value().contains(ARGS_RAW_SERVER_MIN_THREADS))) {
		result.push_back(ARGS_RAW_SERVER_MIN_THREADS);;
		result.push_back(std::to_string(RawServerMinThreads));
	}
	if (LpdPort != DEFAULT_LPD_PORT && (!ignoreItems.has_value() || !ignoreItems.value().contains(ARGS_LPD_PORT))) {
		result.push_back(ARGS_LPD_PORT);
		result.push_back(std::to_string(LpdPort));
	}
	if (LpdDisable != DEFALUT_LPD_DISABLE && (!ignoreItems.has_value() || !ignoreItems.value().contains(ARGS_LPD_DISABLE))) {
		result.push_back(ARGS_LPD_DISABLE);
		result.push_back(" ");
	}
	if (LpdMinThreads != DEFALUT_MIN_THREADS && (!ignoreItems.has_value() || !ignoreItems.value().contains(ARGS_LPD_MIN_THREADS))) {
		result.push_back(ARGS_LPD_MIN_THREADS);;
		result.push_back(std::to_string(LpdMinThreads));
	}
	if (LogLevel != DEFALUT_LOG_LEVEL && (!ignoreItems.has_value() || !ignoreItems.value().contains(ARGS_LOG_LEVEL))) {
		result.push_back(ARGS_LOG_LEVEL);
		result.push_back(logLevelToString(LogLevel));
	}
	if (ServiceOperation.has_value() && (!ignoreItems.has_value() || !ignoreItems.value().contains(ARGS_SERVICE))) {
		result.push_back(ARGS_SERVICE);
		result.push_back(ServiceOperation.value());
	}
	if (Silent != DEFALUT_SILENT && (!ignoreItems.has_value() || !ignoreItems.value().contains(ARGS_SILENT))) {
		result.push_back(ARGS_SILENT);
	}
	return result;
}

void Arguments::PrintHelp() const {
	std::cout << m_parser;
}

std::string Arguments::Dump() const {
	std::string result;
	result.append(fmt::format("RawServersConfig: {}\n", RawServersConfig.has_value() ? "set" : "not set"));
	if (RawServersConfig.has_value()) {
		for (const auto& server : *RawServersConfig) {
			result.append(fmt::format("  - {}:{}\n", std::get<0>(server), std::to_string(std::get<1>(server))));
		}
	}
	result.append(fmt::format("RawServerMinThreads: {}\n", RawServerMinThreads));
	result.append(fmt::format("LpdPort: {}\n", LpdPort));
	result.append(fmt::format("LpdDisable: {}\n", LpdDisable));
	result.append(fmt::format("LpdMinThreads: {}\n", LpdMinThreads));
	result.append(fmt::format("LogLevel: {}\n", logLevelToString(LogLevel)));
	if (ServiceOperation.has_value()) {
		result.append(fmt::format("ServiceOperation: {}\n", ServiceOperation.value()));
	} else {
		result.append("ServiceOperation: not set\n");
	}
	result.append(fmt::format("Silent: {}\n", Silent));
	return result;
}

spdlog::level::level_enum Arguments::parseLogLevel(const std::string& str) {
	using spdlog::level::level_enum;
	static const std::map<std::string, level_enum> mapping{
		{"trace",   level_enum::trace},
		{"debug",   level_enum::debug},
		{"info",    level_enum::info},
		{"warning", level_enum::warn},
		{"error",   level_enum::err},
		{"critical", level_enum::critical},
		{"off",     level_enum::off},
	};
	std::string levelStr;
	for (const auto& c : str) {
		levelStr += std::tolower(c);
	}
	auto it = mapping.find(levelStr);
	if (it != mapping.end()) {
		return it->second;
	}
	auto levelNumber = utility::StringToNumber<int>(levelStr);
	if (!levelNumber.has_value()) {
		throw std::invalid_argument("Invalid log level: " + str);
	}
	switch (levelNumber.value()) {
		case static_cast<int>(level_enum::trace):
			return level_enum::trace;
		case static_cast<int>(level_enum::debug):
			return level_enum::debug;
		case static_cast<int>(level_enum::info):
			return level_enum::info;
		case static_cast<int>(level_enum::warn):
			return level_enum::warn;
		case static_cast<int>(level_enum::err):
			return level_enum::err;
		case static_cast<int>(level_enum::critical):
			return level_enum::critical;
		case static_cast<int>(level_enum::off):
			return level_enum::off;
		default:
			return levelNumber.value() < static_cast<int>(level_enum::trace) ? level_enum::trace : level_enum::off;
	}
}

std::string Arguments::logLevelToString(spdlog::level::level_enum level) {
	switch (level) {
		case spdlog::level::trace:
			return "trace";
		case spdlog::level::debug:
			return "debug";
		case spdlog::level::info:
			return "info";
		case spdlog::level::warn:
			return "warn";
		case spdlog::level::err:
			return "error";
		case spdlog::level::critical:
			return "critical";
		case spdlog::level::off:
			return "off";
		default:
			return "unknown";
	}
}

void Arguments::parseRawServers(const std::vector<std::string>& rawServers) {
	RawServersConfig = std::make_optional(RawServersConfigType{});
	for (const auto& rawServer : rawServers) {
		if (rawServer.empty()) continue;
		std::string printerName;
		uint16_t port = DEFAULT_RAW_PORT;
		auto it = rawServer.find(':');
		if (it != std::string::npos) {
			printerName = rawServer.substr(0, it);
			auto portStr = rawServer.substr(it + 1);
			auto port_ = utility::StringToNumber<uint16_t>(portStr);
			if (port_.has_value() && port_.value() > 0) {
				port = port_.value();
			} else {
				throw std::invalid_argument(fmt::format("Invalid port number in raw server definition: {}", rawServer));
			}
		} else {
			printerName = rawServer;
		}
		if (printerName.empty()) {
			throw std::invalid_argument(fmt::format("Printer name cannot be empty in raw server definition: {}", rawServer));
		}
		RawServersConfig.value().emplace_back(printerName, port);
	}
	if( RawServersConfig.value().empty()) {
		RawServersConfig.reset();
	}
}