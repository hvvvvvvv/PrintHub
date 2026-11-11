find_package(httplib CONFIG REQUIRED)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/include/config.h.in
	${CMAKE_CURRENT_BINARY_DIR}/generated/config.h
)
find_package(nlohmann_json CONFIG REQUIRED)

set(web_assets_source "${CMAKE_CURRENT_BINARY_DIR}/generated/WebAssets.cpp")
add_custom_command(
    OUTPUT "${web_assets_source}"
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
        "-DOUTPUT=${web_assets_source}"
        -P "${PROJECT_SOURCE_DIR}/scripts/embed-file.cmake"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
        "${PROJECT_SOURCE_DIR}/scripts/embed-file.cmake"
    VERBATIM
)
add_library(PrintHubWebAssets STATIC "${web_assets_source}")

add_executable(${EXECUTABLE_NAME}
    CommandLine.cpp
    ConfigurationFile.cpp
    Main.cpp
    PrintRuntime.cpp
    ServiceConfiguration.cpp
    WebServer.cpp
    printer/PrinterJob.cpp
    printer/PrinterMetadataCollector.cpp
    printer/PrinterMetadataProvider.cpp
    printer/UsbPrinterIdentity.cpp
    server/LPD.cpp
    server/LpdSession.cpp
    server/NetworkRuntime.cpp
    server/PpmMib.cpp
    server/PrinterMetadata.cpp
    server/RawPrinting.cpp
    server/RawPrintSession.cpp
    server/Server.cpp
    server/SnmpProtocol.cpp
    server/SnmpServer.cpp
    server/TcpClient.cpp
    utility/service.cpp
)
target_compile_features(${EXECUTABLE_NAME} PRIVATE cxx_std_20)
# Keep Release symbols so scripts/build.ps1 can publish PrintHub.pdb.
target_compile_options(${EXECUTABLE_NAME} PRIVATE "$<$<CONFIG:Release>:/Zi>")
target_link_options(${EXECUTABLE_NAME} PRIVATE "$<$<CONFIG:Release>:/DEBUG:FULL>")
target_link_libraries(${EXECUTABLE_NAME} PRIVATE spdlog::spdlog ws2_32 Winspool Advapi32 Setupapi ole32 uuid nlohmann_json::nlohmann_json httplib::httplib PrintHubWebAssets)
target_compile_definitions(${EXECUTABLE_NAME} PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601)
target_include_directories(${EXECUTABLE_NAME} PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
