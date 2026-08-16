# CMakeList.txt: PrintHub 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required(VERSION 3.12)

# 如果支持，请为 MSVC 编译器启用热重载。
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()
cmake_policy(SET CMP0091 NEW)

project (PrintHub VERSION 1.0.2)
if (NOT WIN32)
  message(FATAL_ERROR "PrintHub is a Windows-only network printing service.")
endif()
find_package(spdlog REQUIRED)

set(EXECUTABLE_NAME ${PROJECT_NAME})
include(CTest)
add_subdirectory(src)
option(BUILD_PRINT_HUB_MANAGER "Build the MFC service management panel" ON)
if(BUILD_PRINT_HUB_MANAGER)
  add_subdirectory(src/gui)
endif()
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
