cmake_minimum_required (VERSION 3.12)
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/include/config.h.in
	${CMAKE_CURRENT_SOURCE_DIR}/include/config.h
)
add_executable(${EXCUTE_NAME})
set_target_properties(${EXCUTE_NAME} PROPERTIES CXX_STANDARD 20)
target_sources(${EXCUTE_NAME} PRIVATE "Main.cpp" "include/argparse.h" "include/Server.h" "include/TcpClient.h" "server/TcpClient.cpp"  "server/Server.cpp"  "server/LPD.cpp" "include/utility.h" "include/PrinterJob.h" "printer/PrinterJob.cpp"  "server/RawPrinting.cpp" "utility/service.cpp"  "include/Arguments.hpp")
target_link_libraries(${EXCUTE_NAME} spdlog::spdlog)
target_link_libraries(${EXCUTE_NAME} ws2_32.lib)
target_link_libraries(${EXCUTE_NAME} Winspool.lib)
target_link_libraries(${EXCUTE_NAME} Advapi32.lib)
include_directories("./include")
