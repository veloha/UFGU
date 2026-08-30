cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED MFG_DLSS_PACKAGE_DIR OR MFG_DLSS_PACKAGE_DIR STREQUAL "")
    message(FATAL_ERROR "Set MFG_DLSS_PACKAGE_DIR to the staged MO2 package root.")
endif()

set(
    expected_files
    "fomod/ModuleConfig.xml"
    "fomod/info.xml"
    "fomod/images/header.png"
    "presets/nvidia/UFGU.ini"
    "presets/amd/UFGU.ini"
    "presets/intel/UFGU.ini"
    "Licenses/CommonLibSSE-NG.txt"
    "Licenses/AMD-Anti-Lag-2.txt"
    "Licenses/PROJECT-LICENSE.txt"
    "Licenses/fmt.txt"
    "Licenses/rapidcsv.txt"
    "Licenses/spdlog.txt"
    "SKSE/Plugins/UFGU.dll"
    "SKSE/Plugins/UFGU.ini"
    "SKSE/Plugins/UFGU/AMD/amd_fidelityfx_upscaler_dx12.dll"
    "SKSE/Plugins/UFGU/AMD/amd_fidelityfx_loader_dx12.dll"
    "SKSE/Plugins/UFGU/AMD/amd_fidelityfx_framegeneration_dx12.dll"
    "SKSE/Plugins/UFGU/AMD/Licenses/AMD-FidelityFX-license.txt"
    "SKSE/Plugins/UFGU/AMD/Licenses/AMD-FidelityFX-third-party.txt"
    "SKSE/Plugins/UFGU/Intel/libxess.dll"
    "SKSE/Plugins/UFGU/Intel/libxess_fg.dll"
    "SKSE/Plugins/UFGU/Intel/libxell.dll"
    "SKSE/Plugins/UFGU/Intel/Licenses/Intel-XeSS-license.txt"
    "SKSE/Plugins/UFGU/Intel/Licenses/Intel-XeSS-third-party.txt"
    "SKSE/Plugins/UFGU/Streamline/nvngx_dlss.dll"
    "SKSE/Plugins/UFGU/Streamline/nvngx_dlssg.dll"
    "SKSE/Plugins/UFGU/Streamline/sl.common.dll"
    "SKSE/Plugins/UFGU/Streamline/sl.dlss.dll"
    "SKSE/Plugins/UFGU/Streamline/sl.dlss_g.dll"
    "SKSE/Plugins/UFGU/Streamline/sl.interposer.dll"
    "SKSE/Plugins/UFGU/Streamline/sl.pcl.dll"
    "SKSE/Plugins/UFGU/Streamline/sl.reflex.dll"
    "SKSE/Plugins/UFGU/Streamline/Licenses/Streamline-license.txt"
    "SKSE/Plugins/UFGU/Streamline/Licenses/Streamline-3rd-party-licenses.txt"
    "SKSE/Plugins/UFGU/Streamline/Licenses/NVIDIA-RTX-SDK-license.txt"
    "SKSE/Plugins/UFGU/Streamline/Licenses/NVIDIA-Reflex-license.txt"
)

set(unique_expected_files ${expected_files})
list(REMOVE_DUPLICATES unique_expected_files)
list(LENGTH expected_files expected_count)
list(LENGTH unique_expected_files unique_expected_count)
if(NOT expected_count EQUAL unique_expected_count)
    message(FATAL_ERROR "Public package manifest contains duplicate paths.")
endif()
if(
    NOT "SKSE/Plugins/UFGU/AMD/amd_fidelityfx_framegeneration_dx12.dll"
        IN_LIST expected_files
    OR NOT "SKSE/Plugins/UFGU/Intel/libxess_fg.dll" IN_LIST expected_files
    OR NOT "SKSE/Plugins/UFGU/Intel/libxell.dll" IN_LIST expected_files
)
    message(FATAL_ERROR "Public package manifest omits a frame-generation dependency.")
endif()
foreach(required_metadata IN ITEMS
    "fomod/info.xml"
    "fomod/ModuleConfig.xml"
    "Licenses/CommonLibSSE-NG.txt"
    "Licenses/AMD-Anti-Lag-2.txt"
    "Licenses/PROJECT-LICENSE.txt"
    "Licenses/fmt.txt"
    "Licenses/spdlog.txt"
    "Licenses/rapidcsv.txt"
    "SKSE/Plugins/UFGU/AMD/Licenses/AMD-FidelityFX-license.txt"
    "SKSE/Plugins/UFGU/AMD/Licenses/AMD-FidelityFX-third-party.txt"
    "SKSE/Plugins/UFGU/Intel/Licenses/Intel-XeSS-license.txt"
    "SKSE/Plugins/UFGU/Intel/Licenses/Intel-XeSS-third-party.txt"
    "SKSE/Plugins/UFGU/Streamline/Licenses/Streamline-license.txt"
    "SKSE/Plugins/UFGU/Streamline/Licenses/Streamline-3rd-party-licenses.txt"
    "SKSE/Plugins/UFGU/Streamline/Licenses/NVIDIA-RTX-SDK-license.txt"
    "SKSE/Plugins/UFGU/Streamline/Licenses/NVIDIA-Reflex-license.txt"
)
    if(NOT required_metadata IN_LIST expected_files)
        message(FATAL_ERROR "Public package manifest omits required metadata: ${required_metadata}")
    endif()
endforeach()

foreach(relative_path IN LISTS expected_files)
    if(relative_path MATCHES "\\.md$")
        message(FATAL_ERROR "Reviewed public manifest must contain zero Markdown files.")
    endif()
endforeach()

function(mfgdlss_is_forbidden_package_path relative_path result_variable)
    string(TOLOWER "${relative_path}" lower_path)
    if(
        lower_path MATCHES "(^|/)[^/]*(enb|reshade)[^/]*(/|$)"
        OR lower_path MATCHES "(^|/)(enblocal\\.ini|enbseries\\.ini|reshade\\.ini)$"
        OR lower_path MATCHES "(^|/)(d3d9|d3d11|dxgi)\\.dll$"
        OR lower_path MATCHES "(^|/)skyrim[^/]*\\.ini$"
    )
        set("${result_variable}" TRUE PARENT_SCOPE)
    else()
        set("${result_variable}" FALSE PARENT_SCOPE)
    endif()
endfunction()

foreach(relative_path IN LISTS expected_files)
    mfgdlss_is_forbidden_package_path("${relative_path}" is_forbidden)
    if(is_forbidden)
        message(
            FATAL_ERROR
            "Reviewed manifest contains ENB/ReShade/game-INI content: ${relative_path}"
        )
    endif()
endforeach()

set(em_dash_files "")
foreach(relative_path IN LISTS expected_files)
    if(NOT relative_path MATCHES "\\.(xml|ini|txt)$")
        continue()
    endif()
    set(absolute_path "${MFG_DLSS_PACKAGE_DIR}/${relative_path}")
    if(NOT EXISTS "${absolute_path}" OR IS_DIRECTORY "${absolute_path}")
        continue()
    endif()
    file(READ "${absolute_path}" file_bytes HEX)
    string(FIND "${file_bytes}" "e28094" em_dash_position)
    if(NOT em_dash_position EQUAL -1)
        list(APPEND em_dash_files "${relative_path}")
    endif()
endforeach()
if(em_dash_files)
    message(
        FATAL_ERROR
        "Em dashes are not permitted in shipped text. Found in: ${em_dash_files}"
    )
endif()

if(DEFINED MFG_DLSS_PACKAGE_MANIFEST_ONLY AND MFG_DLSS_PACKAGE_MANIFEST_ONLY)
    message(STATUS "Validated public package manifest: ${expected_count} reviewed files")
    return()
endif()

set(missing_or_empty_files "")
foreach(relative_path IN LISTS expected_files)
    set(absolute_path "${MFG_DLSS_PACKAGE_DIR}/${relative_path}")
    if(NOT EXISTS "${absolute_path}" OR IS_DIRECTORY "${absolute_path}")
        list(APPEND missing_or_empty_files "${relative_path}")
        continue()
    endif()
    file(SIZE "${absolute_path}" file_size)
    if(file_size EQUAL 0)
        list(APPEND missing_or_empty_files "${relative_path}")
    endif()
endforeach()

file(
    GLOB_RECURSE actual_files
    LIST_DIRECTORIES false
    RELATIVE "${MFG_DLSS_PACKAGE_DIR}"
    "${MFG_DLSS_PACKAGE_DIR}/*"
)
set(unexpected_files "")
set(forbidden_files "")
set(markdown_files "")
foreach(relative_path IN LISTS actual_files)
    file(TO_CMAKE_PATH "${relative_path}" normalized_path)
    if(NOT normalized_path IN_LIST expected_files)
        list(APPEND unexpected_files "${normalized_path}")
    endif()

    mfgdlss_is_forbidden_package_path("${normalized_path}" is_forbidden)
    if(is_forbidden)
        list(APPEND forbidden_files "${normalized_path}")
    endif()
    if(normalized_path MATCHES "\\.md$")
        list(APPEND markdown_files "${normalized_path}")
    endif()
endforeach()

if(missing_or_empty_files)
    message(
        FATAL_ERROR
        "Public package is missing required non-empty files: ${missing_or_empty_files}"
    )
endif()
if(forbidden_files)
    message(
        FATAL_ERROR
        "Public package must never contain ENB/ReShade/game-INI files: ${forbidden_files}"
    )
endif()
if(markdown_files)
    message(FATAL_ERROR "Public package must contain zero Markdown files: ${markdown_files}")
endif()
if(unexpected_files)
    message(
        FATAL_ERROR
        "Public package contains files outside the reviewed manifest: ${unexpected_files}"
    )
endif()

set(ufgu_plugin_path "${MFG_DLSS_PACKAGE_DIR}/SKSE/Plugins/UFGU.dll")
file(STRINGS "${ufgu_plugin_path}" ufgu_plugin_strings LENGTH_MINIMUM 4)
set(embedded_project_paths "")
set(embedded_vendor_paths "")
foreach(plugin_string IN LISTS ufgu_plugin_strings)
    string(
        REGEX MATCHALL
        "[A-Za-z]:[/\\\\][A-Za-z0-9_ ./\\\\-]+\\.(cpp|cxx|cc|c|hpp|hxx|hh|h|inl|ixx|asm|hlsl|pdb)"
        absolute_source_paths
        "${plugin_string}"
    )
    foreach(absolute_source_path IN LISTS absolute_source_paths)
        if(absolute_source_path MATCHES "^C:/dvs/p4/build/sw/devrel/libdev/NGX/")
            list(APPEND embedded_vendor_paths "${absolute_source_path}")
        else()
            list(APPEND embedded_project_paths "${absolute_source_path}")
        endif()
    endforeach()
endforeach()
if(embedded_project_paths)
    list(REMOVE_DUPLICATES embedded_project_paths)
    message(
        FATAL_ERROR
        "UFGU.dll contains unexpected absolute build or source paths: ${embedded_project_paths}"
    )
endif()
if(embedded_vendor_paths)
    list(REMOVE_DUPLICATES embedded_vendor_paths)
    list(LENGTH embedded_vendor_paths embedded_vendor_path_count)
    message(STATUS "Recognized ${embedded_vendor_path_count} NVIDIA NGX build-path strings")
endif()

list(LENGTH actual_files actual_count)
message(
    STATUS
    "Validated self-contained MO2 package: ${actual_count}/${expected_count} reviewed files"
)
