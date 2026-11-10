#pragma once
#include <windows.h>
#include <winspool.h>
#include <string> 
#include <optional>
#include <vector>
#include <cstdint>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <winsvc.h>

namespace utility {
	template <typename T>
	inline std::optional<T> StringToNumber(const std::string& s) {
		// 确保类型 T 是 std::from_chars 支持的类型
		static_assert(
			std::is_integral_v<T> || std::is_floating_point_v<T>,
			"T must be an integral or floating-point type"
			);

		T value{};
		auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);

		// 检查是否成功转换且消费了整个字符串
		if (s.size() > 0 && ec == std::errc() && ptr == s.data() + s.size()) {
			return value;
		}

		return std::nullopt;  // 转换失败
	}

	inline std::filesystem::path getExecutablePath() {
		CHAR path[MAX_PATH];
		const auto length = GetModuleFileNameA(nullptr, path, MAX_PATH);
		if (length == 0 || length >= MAX_PATH) {
			return std::filesystem::current_path();
		}
		return std::filesystem::path(std::string(path, length));
	}

	inline std::filesystem::path getExecutableDirectory() {
		return getExecutablePath().parent_path();
	}

	inline bool IsRunAsAdmin() {
		BOOL isAdmin = FALSE;
		HANDLE hToken = nullptr;

		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
			return false;
		TOKEN_ELEVATION elevation;
		DWORD dwSize;
		if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) {
			isAdmin = elevation.TokenIsElevated;
		}

		CloseHandle(hToken);
		return isAdmin != FALSE;
	}


	class ServiceManager {		
		public:
			struct InstallArgs {
				std::string binaryPath;
				std::vector<std::string> arguments;
				std::vector<std::string> dependencies;
				std::optional<std::string> displayName;
			};
			ServiceManager(const std::string& serviceName);
			ServiceManager() = delete;
			ServiceManager(const ServiceManager&) = delete;
			ServiceManager& operator=(const ServiceManager&) = delete;
			~ServiceManager();
			void StartSrv();
			void StopSrv();
			void RestartSrv();
			void InstallSrv(const InstallArgs &args);
			void UninstallSrv();
			bool IsRunning() const;
			bool IsInstalled() const;
		private:
			std::string m_serviceName;
			SC_HANDLE m_serviceHandle;
	};
}
