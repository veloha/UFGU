set(
    STREAMLINE_SDK_ROOT
    ""
    CACHE PATH
    "Path to an unpacked NVIDIA Streamline SDK"
)

if(NOT STREAMLINE_SDK_ROOT)
    message(FATAL_ERROR "Set STREAMLINE_SDK_ROOT to the unpacked NVIDIA Streamline SDK.")
endif()

find_path(
    STREAMLINE_INCLUDE_DIR
    NAMES sl.h
    PATHS "${STREAMLINE_SDK_ROOT}/include"
    NO_DEFAULT_PATH
    REQUIRED
)

find_path(
    NVIDIA_NGX_INCLUDE_DIR
    NAMES nvsdk_ngx.h
    PATHS "${STREAMLINE_SDK_ROOT}/external/ngx-sdk/include"
    NO_DEFAULT_PATH
    REQUIRED
)

find_library(
    NVIDIA_NGX_LIBRARY
    NAMES nvsdk_ngx_d
    PATHS "${STREAMLINE_SDK_ROOT}/external/ngx-sdk/lib/Windows_x86_64"
    NO_DEFAULT_PATH
    REQUIRED
)

set(STREAMLINE_RUNTIME_DIR "${STREAMLINE_SDK_ROOT}/bin/x64")

if(NOT EXISTS "${STREAMLINE_RUNTIME_DIR}/sl.interposer.dll")
    message(FATAL_ERROR "Streamline runtime binaries were not found under ${STREAMLINE_RUNTIME_DIR}.")
endif()

if(NOT TARGET Streamline::Headers)
    add_library(Streamline::Headers INTERFACE IMPORTED)
    set_target_properties(
        Streamline::Headers
        PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${STREAMLINE_INCLUDE_DIR}"
    )
endif()

if(NOT TARGET NVIDIA::NGX)
    add_library(NVIDIA::NGX INTERFACE IMPORTED)
    set_target_properties(
        NVIDIA::NGX
        PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${NVIDIA_NGX_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${NVIDIA_NGX_LIBRARY}"
    )
endif()

function(mfgdlss_copy_required_streamline_runtime target destination)
    set(
        required_runtime_files
        nvngx_dlss.dll
        nvngx_dlssg.dll
        sl.common.dll
        sl.dlss.dll
        sl.dlss_g.dll
        sl.interposer.dll
        sl.pcl.dll
        sl.reflex.dll
    )

    foreach(runtime_name IN LISTS required_runtime_files)
        set(runtime_file "${STREAMLINE_RUNTIME_DIR}/${runtime_name}")
        if(NOT EXISTS "${runtime_file}")
            message(FATAL_ERROR "Required Streamline runtime file is missing: ${runtime_file}")
        endif()
        add_custom_command(
            TARGET "${target}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${destination}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_file}"
                    "${destination}"
            VERBATIM
        )
    endforeach()

    set(
        required_license_files
        "${STREAMLINE_SDK_ROOT}/license.txt|Streamline-license.txt"
        "${STREAMLINE_SDK_ROOT}/3rd-party-licenses.md|Streamline-3rd-party-licenses.txt"
        "${STREAMLINE_RUNTIME_DIR}/nvngx_dlss.license.txt|NVIDIA-RTX-SDK-license.txt"
        "${STREAMLINE_RUNTIME_DIR}/reflex.license.txt|NVIDIA-Reflex-license.txt"
    )

    foreach(license_mapping IN LISTS required_license_files)
        string(REPLACE "|" ";" license_parts "${license_mapping}")
        list(GET license_parts 0 license_source)
        list(GET license_parts 1 license_name)
        if(NOT EXISTS "${license_source}")
            message(FATAL_ERROR "Required Streamline license file is missing: ${license_source}")
        endif()
        add_custom_command(
            TARGET "${target}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${destination}/Licenses"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${license_source}"
                    "${destination}/Licenses/${license_name}"
            VERBATIM
        )
    endforeach()
endfunction()
