#include <NetworkRuntime.h>

#include <system_error>

#include <winsock2.h>

namespace {

constexpr BYTE kWinsockMajorVersion = 2;
constexpr BYTE kWinsockMinorVersion = 2;

} // namespace

WinsockRuntime::WinsockRuntime() {
    WSADATA data{};
    const auto error = WSAStartup(MAKEWORD(kWinsockMajorVersion, kWinsockMinorVersion), &data);
    if (error != 0) {
        throw std::system_error(error, std::system_category(), "WSAStartup failed");
    }
}

WinsockRuntime::~WinsockRuntime() {
    WSACleanup();
}
