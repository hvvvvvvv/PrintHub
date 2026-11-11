#pragma once

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

inline void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Exception = std::invalid_argument, typename Function>
void requireThrows(Function&& function, std::string_view message) {
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

template <typename Function>
void runTest(std::string_view name, Function&& function) {
    try {
        std::forward<Function>(function)();
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(name) + ": " + error.what());
    }
}

inline std::string bytes(std::initializer_list<std::uint8_t> values) {
    std::string result;
    for (const auto value : values) result.push_back(static_cast<char>(value));
    return result;
}

} // namespace
