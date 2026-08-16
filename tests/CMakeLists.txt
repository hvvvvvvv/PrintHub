add_executable(PrintHubProtocolTests
    ProtocolSessionTests.cpp
    ${PROJECT_SOURCE_DIR}/src/server/LpdSession.cpp
    ${PROJECT_SOURCE_DIR}/src/server/RawPrintSession.cpp
    ${PROJECT_SOURCE_DIR}/src/server/SnmpRejectSession.cpp
    ${PROJECT_SOURCE_DIR}/src/ServiceConfiguration.cpp
)

target_include_directories(PrintHubProtocolTests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/include
)
target_link_libraries(PrintHubProtocolTests PRIVATE spdlog::spdlog)
set_target_properties(PrintHubProtocolTests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

add_test(NAME PrintHubProtocolTests COMMAND PrintHubProtocolTests)

add_executable(PrintHubConfigurationTests
    ConfigurationTests.cpp
    ${PROJECT_SOURCE_DIR}/src/ManagerCommands.cpp
    ${PROJECT_SOURCE_DIR}/src/ServiceConfiguration.cpp
)
target_include_directories(PrintHubConfigurationTests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/include
)
set_target_properties(PrintHubConfigurationTests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
add_test(NAME PrintHubConfigurationTests COMMAND PrintHubConfigurationTests)
