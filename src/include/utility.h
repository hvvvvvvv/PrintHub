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
#include <stdexcept>
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

	inline std::filesystem::path getExecutablePathW() {
		std::vector<wchar_t> path(MAX_PATH);
		for (;;) {
			const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
			if (length == 0) {
				const auto error = GetLastError();
				throw std::runtime_error("Failed to get executable path (Win32 error " + std::to_string(error) + ")");
			}
			if (length < path.size()) {
				return std::filesystem::path(std::wstring(path.data(), length));
			}
			if (path.size() > MAXDWORD / 2 || path.size() > path.max_size() / 2) {
				throw std::runtime_error("Failed to get executable path (Win32 error " +
					std::to_string(ERROR_INSUFFICIENT_BUFFER) + ")");
			}
			path.resize(path.size() * 2);
		}
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
				std::filesystem::path binaryPath;
				std::vector<std::wstring> arguments;
				std::vector<std::wstring> dependencies;
				std::optional<std::wstring> displayName;
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
