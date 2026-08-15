add_executable(PrintHubProtocolTests
    ProtocolSessionTests.cpp
    ${PROJECT_SOURCE_DIR}/src/server/LpdSession.cpp
    ${PROJECT_SOURCE_DIR}/src/server/RawPrintSession.cpp
    ${PROJECT_SOURCE_DIR}/src/server/SnmpRejectSession.cpp
)

target_include_directories(PrintHubProtocolTests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/include
)
target_link_libraries(PrintHubProtocolTests PRIVATE spdlog::spdlog)
set_target_properties(PrintHubProtocolTests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

add_test(NAME PrintHubProtocolTests COMMAND PrintHubProtocolTests)
