# Keep every suite independent of the GUI, listeners and real printer backends.
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

printhub_add_test(PrintHubConfigurationTests
    ServiceConfigurationTests.cpp
    ${PROJECT_SOURCE_DIR}/src/ServiceConfiguration.cpp
)
target_link_libraries(PrintHubConfigurationTests PRIVATE shell32)

printhub_add_test(PrintHubManagerCommandsTests
    ManagerCommandsTests.cpp
    ${PROJECT_SOURCE_DIR}/src/ManagerCommands.cpp
    ${PROJECT_SOURCE_DIR}/src/ServiceConfiguration.cpp
)

printhub_add_test(PrintHubArgumentsTests
    ArgumentsTests.cpp
    ${PROJECT_SOURCE_DIR}/src/Arguments.cpp
    ${PROJECT_SOURCE_DIR}/src/ServiceConfiguration.cpp
)
# src/CMakeLists.txt already configures this header before adding tests.
target_include_directories(PrintHubArgumentsTests PRIVATE ${PROJECT_BINARY_DIR}/src/generated)
target_link_libraries(PrintHubArgumentsTests PRIVATE spdlog::spdlog)

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
