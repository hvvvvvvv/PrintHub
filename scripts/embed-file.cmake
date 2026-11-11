if(NOT DEFINED INPUT OR INPUT STREQUAL "" OR NOT EXISTS "${INPUT}" OR IS_DIRECTORY "${INPUT}")
    message(FATAL_ERROR "INPUT must name an existing, non-empty file")
endif()
if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
    message(FATAL_ERROR "OUTPUT must name the generated C++ file")
endif()

file(READ "${INPUT}" input_hex HEX)
if(input_hex STREQUAL "")
    message(FATAL_ERROR "Cannot embed an empty file: ${INPUT}")
endif()

get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
if(output_directory STREQUAL "")
    set(output_directory ".")
endif()
file(MAKE_DIRECTORY "${output_directory}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1,\n" input_bytes "${input_hex}")

# Explicit external linkage lets both the application and tests use the archive.
file(WRITE "${OUTPUT}"
    "// Generated from ${INPUT}. Do not edit.\n"
    "#include <cstddef>\n"
    "extern const unsigned char kWebAssetsIndexHtmlData[] = {\n"
    "${input_bytes}"
    "};\n"
    "extern const std::size_t kWebAssetsIndexHtmlSize = sizeof(kWebAssetsIndexHtmlData);\n"
)
