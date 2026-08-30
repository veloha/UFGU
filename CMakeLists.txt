cmake_minimum_required(VERSION 3.25)

project(
    UFGU
    VERSION 0.1.0
    DESCRIPTION "Universal frame generation and upscaling for Skyrim"
    LANGUAGES CXX RC
)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MFG_DLSS_BUILD_SKSE_PLUGIN "Build the native SKSE plugin" ON)
option(
    MFG_DLSS_ENABLE_MSVC_CODE_ANALYSIS
    "Run MSVC native code analysis on project-owned plugin translation units"
    OFF
)
option(
    MFG_DLSS_PUBLIC_RELEASE_BUILD
    "Enable public-release safeguards; forbids NVIDIA's sample application ID"
    OFF
)
set(
    MFG_DLSS_PUBLIC_PACKAGE_DIR
    "${CMAKE_BINARY_DIR}/package"
    CACHE PATH
    "Output directory for the self-contained MO2 public package"
)
set(
    MFG_DLSS_PROJECT_LICENSE_FILE
    "${PROJECT_SOURCE_DIR}/LICENSE"
    CACHE FILEPATH
    "Project license copied into public packages"
)

set(
    MFG_DLSS_STREAMLINE_APPLICATION_ID
    "0"
    CACHE STRING
    "NVIDIA-issued Streamline application ID; 0 uses the project-ID/engine path"
)

if(NOT MFG_DLSS_STREAMLINE_APPLICATION_ID MATCHES "^[0-9]+$")
    message(FATAL_ERROR "MFG_DLSS_STREAMLINE_APPLICATION_ID must be an unsigned decimal integer.")
endif()

if(
    MFG_DLSS_PUBLIC_RELEASE_BUILD
    AND MFG_DLSS_STREAMLINE_APPLICATION_ID STREQUAL "231313132"
)
    message(
        FATAL_ERROR
        "Public-release builds cannot use NVIDIA's sample application ID 231313132. "
        "Use 0 for the project's GUID/engine identity or configure an ID issued "
        "to this project by NVIDIA."
    )
endif()

if(NOT WIN32)
    message(FATAL_ERROR "UFGU targets Windows because Skyrim, ENB, and DirectX are Windows-native.")
endif()

if(MSVC)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    add_compile_options(
        /W4
        /WX
        /permissive-
        /EHsc
        /utf-8
        /experimental:deterministic
        "/pathmap:${CMAKE_SOURCE_DIR}=UFGU"
        "/pathmap:${CMAKE_BINARY_DIR}=build"
    )
    add_link_options(/Brepro)
endif()

if(MFG_DLSS_BUILD_SKSE_PLUGIN)
    include("${PROJECT_SOURCE_DIR}/cmake/Streamline.cmake")
endif()

set(
    MFG_DLSS_PLUGIN_SOURCE_DIR
    "src"
    CACHE STRING
    "Project-relative directory holding the plugin sources to build"
)

if(MFG_DLSS_BUILD_SKSE_PLUGIN)
    add_subdirectory("${MFG_DLSS_PLUGIN_SOURCE_DIR}")
endif()
