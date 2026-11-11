# Keep every suite independent of the listeners and real printer backends.
function(printhub_add_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src/include)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
    target_compile_definitions(${target} PRIVATE NOMINMAX WINVER=0x0601 _WIN32_WINNT=0x0601)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 5)
endfunction()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)

printhub_add_test(PrintHubConfigurationTests
    ServiceConfigurationTests.cpp
    ${PROJECT_SOURCE_DIR}/src/ServiceConfiguration.cpp
)

printhub_add_test(PrintHubConfigurationFileTests
    ConfigurationFileTests.cpp
    ${PROJECT_SOURCE_DIR}/src/ConfigurationFile.cpp
    ${PROJECT_SOURCE_DIR}/src/ServiceConfiguration.cpp
)
target_link_libraries(PrintHubConfigurationFileTests PRIVATE nlohmann_json::nlohmann_json)

printhub_add_test(PrintHubCommandLineTests
    CommandLineTests.cpp
    ${PROJECT_SOURCE_DIR}/src/CommandLine.cpp
)
target_link_libraries(PrintHubCommandLineTests PRIVATE shell32)

printhub_add_test(PrintHubAsciiTextTests AsciiTextTests.cpp)
printhub_add_test(PrintHubBoundedQueueTests BoundedQueueTests.cpp)
printhub_add_test(PrintHubSnmpBerTests SnmpBerTests.cpp)

printhub_add_test(PrintHubLpdSessionTests
    LpdSessionTests.cpp
    ${PROJECT_SOURCE_DIR}/src/server/LpdSession.cpp
    ${PROJECT_SOURCE_DIR}/src/ServiceConfiguration.cpp
)
target_link_libraries(PrintHubLpdSessionTests PRIVATE spdlog::spdlog)

printhub_add_test(PrintHubRawPrintSessionTests
    RawPrintSessionTests.cpp
    ${PROJECT_SOURCE_DIR}/src/server/RawPrintSession.cpp
)

printhub_add_test(PrintHubSnmpProtocolTests
    SnmpProtocolTests.cpp
    ${PROJECT_SOURCE_DIR}/src/server/SnmpProtocol.cpp
)

printhub_add_test(PrintHubPpmMibTests
    PpmMibTests.cpp
    ${PROJECT_SOURCE_DIR}/src/server/PpmMib.cpp
    ${PROJECT_SOURCE_DIR}/src/server/PrinterMetadata.cpp
    ${PROJECT_SOURCE_DIR}/src/server/SnmpProtocol.cpp
    ${PROJECT_SOURCE_DIR}/src/ServiceConfiguration.cpp
)

printhub_add_test(PrintHubWebApiTests
    WebApiTests.cpp
    ${PROJECT_SOURCE_DIR}/src/WebServer.cpp
)
target_link_libraries(PrintHubWebApiTests PRIVATE httplib::httplib PrintHubWebAssets nlohmann_json::nlohmann_json ws2_32)
set_tests_properties(PrintHubWebApiTests PROPERTIES TIMEOUT 30)
