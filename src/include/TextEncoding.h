#pragma once

#include <climits>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include <Windows.h>

inline std::wstring Utf8ToWide(std::string_view value) {
    if (value.size() > static_cast<size_t>(INT_MAX)) {
        throw std::invalid_argument("UTF-8 input exceeds INT_MAX");
    }
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (count == 0) throw std::invalid_argument("Invalid UTF-8 text");
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(), count) != count) {
        throw std::invalid_argument("Invalid UTF-8 text");
    }
    return result;
}

inline std::string WideToUtf8(std::wstring_view value) {
    if (value.size() > static_cast<size_t>(INT_MAX)) {
        throw std::invalid_argument("UTF-16 input exceeds INT_MAX");
    }
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
        nullptr, 0, nullptr, nullptr);
    if (count == 0) throw std::invalid_argument("Invalid UTF-16 text");
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
        result.data(), count, nullptr, nullptr) != count) {
        throw std::invalid_argument("Invalid UTF-16 text");
    }
    return result;
}

inline std::string WideToAcp(std::wstring_view value) {
    if (value.size() > static_cast<size_t>(INT_MAX)) {
        throw std::invalid_argument("UTF-16 input exceeds INT_MAX");
    }
    if (value.empty()) return {};
    // UTF-8 ACP forbids WC_NO_BEST_FIT_CHARS and lpUsedDefaultChar.
    if (GetACP() == CP_UTF8) return WideToUtf8(value);
    const auto length = static_cast<int>(value.size());
    BOOL usedDefault = FALSE;
    const auto count = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, value.data(), length,
        nullptr, 0, nullptr, &usedDefault);
    if (count == 0 || usedDefault) {
        throw std::invalid_argument("Text cannot be encoded in the system ANSI code page");
    }
    std::string result(static_cast<size_t>(count), '\0');
    usedDefault = FALSE;
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, value.data(), length,
        result.data(), count, nullptr, &usedDefault) != count || usedDefault) {
        throw std::invalid_argument("Text cannot be encoded in the system ANSI code page");
    }
    return result;
}

inline std::string Utf8ToAcp(std::string_view value) {
    return WideToAcp(Utf8ToWide(value));
}

inline std::string AcpToUtf8(std::string_view value) {
    if (value.size() > static_cast<size_t>(INT_MAX)) {
        throw std::invalid_argument("ANSI input exceeds INT_MAX");
    }
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const auto count = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (count == 0) throw std::invalid_argument("Invalid system ANSI code page text");
    std::wstring wide(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, value.data(), length, wide.data(), count) != count) {
        throw std::invalid_argument("Invalid system ANSI code page text");
    }
    return WideToUtf8(wide);
}
