configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/include/config.h.in
	${CMAKE_CURRENT_BINARY_DIR}/generated/config.h
)
add_executable(${EXECUTABLE_NAME}
    Arguments.cpp
    Main.cpp
    ServiceConfiguration.cpp
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
    server/SnmpRejectSession.cpp
    server/SnmpProtocol.cpp
    server/SnmpServer.cpp
    server/TcpClient.cpp
    utility/service.cpp
)
target_compile_features(${EXECUTABLE_NAME} PRIVATE cxx_std_20)
target_link_libraries(${EXECUTABLE_NAME} PRIVATE spdlog::spdlog ws2_32 Winspool Advapi32 Setupapi ole32 uuid)
target_compile_definitions(${EXECUTABLE_NAME} PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601)
target_include_directories(${EXECUTABLE_NAME} PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
