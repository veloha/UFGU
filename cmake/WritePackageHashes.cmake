cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED MFG_DLSS_PACKAGE_DIR OR MFG_DLSS_PACKAGE_DIR STREQUAL "")
    message(FATAL_ERROR "Set MFG_DLSS_PACKAGE_DIR.")
endif()

if(NOT IS_DIRECTORY "${MFG_DLSS_PACKAGE_DIR}")
    message(FATAL_ERROR "Package directory not found: ${MFG_DLSS_PACKAGE_DIR}")
endif()

if(NOT DEFINED MFG_DLSS_HASH_OUTPUT OR MFG_DLSS_HASH_OUTPUT STREQUAL "")
    message(FATAL_ERROR "Set MFG_DLSS_HASH_OUTPUT.")
endif()

file(
    GLOB_RECURSE package_files
    LIST_DIRECTORIES false
    RELATIVE "${MFG_DLSS_PACKAGE_DIR}"
    "${MFG_DLSS_PACKAGE_DIR}/*"
)
list(SORT package_files)

set(hash_lines "")
foreach(relative_path IN LISTS package_files)
    file(SHA256 "${MFG_DLSS_PACKAGE_DIR}/${relative_path}" file_hash)
    string(APPEND hash_lines "${file_hash}  ${relative_path}\n")
endforeach()

file(WRITE "${MFG_DLSS_HASH_OUTPUT}" "${hash_lines}")
list(LENGTH package_files package_count)
message(STATUS "Wrote ${package_count} package hashes to ${MFG_DLSS_HASH_OUTPUT}")
