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
	inline std::optional<std::string> GetDefaultPrinterName() {
		DWORD size = 0;  
		GetDefaultPrinterA(nullptr, &size);
		if (size == 0) {  
			return std::nullopt;
		}  
		std::string printerName(size, '\0');
		if (GetDefaultPrinterA(printerName.data(), &size) && size > 0) {
			printerName.pop_back();
			return printerName;
		}  
		return std::nullopt;
	}

	inline bool IsPrinterAvailable(const std::string& printerName) {  
		HANDLE hPrinter;  
		if (OpenPrinterA(const_cast<char*>(printerName.c_str()), &hPrinter, nullptr)) {  
			ClosePrinter(hPrinter);  
			return true;  
		}  
		return false;  
	}

	inline std::optional<std::string> GetPrinterNameFromShareName(const std::string& shareName) {
		DWORD needed = 0, returned = 0;
		DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
		EnumPrintersA(flags, nullptr, 2, nullptr, 0, &needed, &returned);
		if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
			return std::nullopt;
		}
		std::vector<BYTE> buffer(needed);
		if (!EnumPrintersA(flags, nullptr, 2, buffer.data(), needed, &needed, &returned)) {
			return std::nullopt;
		}

		auto* printers = reinterpret_cast<PRINTER_INFO_2A*>(buffer.data());
		for (DWORD i = 0; i < returned; ++i) {
			if (printers[i].pPrinterName && printers[i].pShareName && shareName == printers[i].pShareName) {
				return std::string(printers[i].pPrinterName);
			}
			if (printers[i].pPrinterName && shareName == printers[i].pPrinterName) {
				return std::string(printers[i].pPrinterName);
			}
		}
		return std::nullopt;
	}

	inline std::vector<std::string> SplistByLine(const std::string_view str) {
		std::vector<std::string> result;

		auto it = str.begin();
		while (it != str.end()) {
			// 找到下一个换行符
			auto end_it = std::find_if(it, str.end(), [](char c) {
				return c == '\n';
				});

			// 添加非空子串
			if (it != end_it) {
				result.emplace_back(it, end_it);
			}

			// 跳过换行符
			if (end_it != str.end()) {
				++end_it;
			}
			it = end_it;
		}

		return result;
	}

	inline std::optional<uint64_t> StringToUint64(const std::string& s) {
		uint64_t value = 0;
		auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
		// 成功且全部字符都消费掉
		if (ec == std::errc() && ptr == s.data() + s.size())
			return value;
		return std::nullopt; // 转换失败
	}

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

	inline std::filesystem::path getExecutableDirectory() {
		try {
			// 获取程序路径
			CHAR path[MAX_PATH];
			GetModuleFileNameA(nullptr, path, MAX_PATH);
			return std::filesystem::path(path).parent_path();
		}
		catch (...) {
			// 如果获取失败，使用当前工作目录
			return std::filesystem::current_path();
		}
	}

	inline std::filesystem::path getExecutablePath() {
		try {
			CHAR path[MAX_PATH];
			GetModuleFileNameA(nullptr, path, MAX_PATH);
			return std::filesystem::path(path);
		}
		catch (...) {
			return std::filesystem::current_path();
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
				std::string binaryPath;
				std::vector<std::string> arguments;
				std::vector<std::string> dependencies;
				std::optional<std::string> displayName;
				std::optional<std::string> description;
				bool startAutomatic = true;
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
