# Keep the MFC dependency local to the manager target: PrintHub itself remains
# a plain Win32 service executable.
enable_language(RC)
set(CMAKE_MFC_FLAG 1)
find_package(MFC REQUIRED)

add_executable(PrintHubManager WIN32
    ManagerApp.cpp
    ManagerDialog.cpp
    PrintHubManager.rc
    ${PROJECT_SOURCE_DIR}/src/ManagerCommands.cpp
    ${PROJECT_SOURCE_DIR}/src/ServiceConfiguration.cpp
)
target_compile_features(PrintHubManager PRIVATE cxx_std_20)
target_compile_options(PrintHubManager PRIVATE
    /utf-8
    "$<$<CONFIG:Release>:/Zi>"
)
target_compile_definitions(PrintHubManager PRIVATE
    UNICODE _UNICODE WINVER=0x0601 _WIN32_WINNT=0x0601
)
target_include_directories(PrintHubManager PRIVATE
    ${PROJECT_SOURCE_DIR}/src/include
)
target_link_libraries(PrintHubManager PRIVATE
    Advapi32 Winspool Shell32
)
target_link_options(PrintHubManager PRIVATE
    "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
    "$<$<CONFIG:Release>:/DEBUG:FULL>"
)
set_target_properties(PrintHubManager PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols"
)

# Ship the manager and the service together so the default path is usable
# without an extra browse step.
add_dependencies(PrintHubManager ${EXECUTABLE_NAME})
add_custom_command(TARGET PrintHubManager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:${EXECUTABLE_NAME}>
        $<TARGET_FILE_DIR:PrintHubManager>
)
