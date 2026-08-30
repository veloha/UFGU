set(
    COMMONLIBSSE_NG_ROOT
    ""
    CACHE PATH
    "Path to a CommonLibSSE-NG source checkout"
)

if(NOT COMMONLIBSSE_NG_ROOT)
    message(FATAL_ERROR "Set COMMONLIBSSE_NG_ROOT when MFG_DLSS_BUILD_SKSE_PLUGIN is enabled.")
endif()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)

set(UFGU_COMPATIBLE_RUNTIMES 1.5.97.0 1.6.1170.0)

add_subdirectory(
    "${COMMONLIBSSE_NG_ROOT}"
    "${CMAKE_BINARY_DIR}/_deps/CommonLibSSE-NG"
    EXCLUDE_FROM_ALL
)
include("${COMMONLIBSSE_NG_ROOT}/cmake/CommonLibSSE.cmake")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.hpp"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/UFGU.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/UFGU.rc"
    @ONLY
)

set(
    MFG_DLSS_AMD_FIDELITYFX_ROOT
    "${PROJECT_SOURCE_DIR}/external/ffx230/src/FidelityFX-SDK-2.3.0"
    CACHE PATH
    "AMD FidelityFX SDK root"
)
set(
    MFG_DLSS_AMD_ANTILAG_ROOT
    "${PROJECT_SOURCE_DIR}/external/amd-antilag2-sdk/extracted/v2.0.0a"
    CACHE PATH
    "AMD Anti-Lag 2 SDK root"
)
set(
    MFG_DLSS_INTEL_XESS_ROOT
    "${PROJECT_SOURCE_DIR}/external/intel-xess-3.0.2/extracted"
    CACHE PATH
    "Intel XeSS SDK root containing the signed runtime and notices"
)
set(
    MFG_DLSS_AMD_RUNTIME_DIR
    "${MFG_DLSS_AMD_FIDELITYFX_ROOT}/Kits/FidelityFX/signedbin"
    CACHE PATH
    "Directory containing AMD's signed FidelityFX D3D12 redistributables"
)
set(
    MFG_DLSS_AMD_LICENSE_DIR
    "${MFG_DLSS_AMD_FIDELITYFX_ROOT}"
    CACHE PATH
    "FidelityFX SDK root containing license.md and 3rdpartynotice.md"
)

function(mfgdlss_stage_vendor_runtimes target destination)
    set(required_files
        "${MFG_DLSS_AMD_RUNTIME_DIR}/amd_fidelityfx_upscaler_dx12.dll"
        "${MFG_DLSS_AMD_RUNTIME_DIR}/amd_fidelityfx_loader_dx12.dll"
        "${MFG_DLSS_AMD_RUNTIME_DIR}/amd_fidelityfx_framegeneration_dx12.dll"
        "${MFG_DLSS_AMD_LICENSE_DIR}/docs/license.md"
        "${MFG_DLSS_AMD_LICENSE_DIR}/3rdpartynotice.md"
        "${MFG_DLSS_INTEL_XESS_ROOT}/bin/libxess.dll"
        "${MFG_DLSS_INTEL_XESS_ROOT}/bin/libxess_fg.dll"
        "${MFG_DLSS_INTEL_XESS_ROOT}/bin/libxell.dll"
        "${MFG_DLSS_INTEL_XESS_ROOT}/LICENSE.txt"
        "${MFG_DLSS_INTEL_XESS_ROOT}/third-party-programs.txt"
    )
    set(missing_files "")
    foreach(file IN LISTS required_files)
        if(NOT EXISTS "${file}")
            list(APPEND missing_files "${file}")
        endif()
    endforeach()
    if(missing_files)
        if(MFG_DLSS_PUBLIC_RELEASE_BUILD)
            message(FATAL_ERROR
                "A public multi-vendor package is missing required signed runtimes/notices: ${missing_files}"
            )
        endif()
        message(WARNING
            "FSR/XeSS runtime staging is disabled because these files are missing: ${missing_files}"
        )
        return()
    endif()

    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${destination}/AMD/Licenses"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${destination}/Intel/Licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MFG_DLSS_AMD_RUNTIME_DIR}/amd_fidelityfx_upscaler_dx12.dll"
                "${destination}/AMD"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MFG_DLSS_AMD_RUNTIME_DIR}/amd_fidelityfx_loader_dx12.dll"
                "${destination}/AMD"

        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MFG_DLSS_AMD_RUNTIME_DIR}/amd_fidelityfx_framegeneration_dx12.dll"
                "${destination}/AMD"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MFG_DLSS_AMD_LICENSE_DIR}/docs/license.md"
                "${destination}/AMD/Licenses/AMD-FidelityFX-license.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MFG_DLSS_AMD_LICENSE_DIR}/3rdpartynotice.md"
                "${destination}/AMD/Licenses/AMD-FidelityFX-third-party.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MFG_DLSS_INTEL_XESS_ROOT}/bin/libxess.dll"
                "${destination}/Intel"

        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MFG_DLSS_INTEL_XESS_ROOT}/bin/libxess_fg.dll"
                "${destination}/Intel"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MFG_DLSS_INTEL_XESS_ROOT}/bin/libxell.dll"
                "${destination}/Intel"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MFG_DLSS_INTEL_XESS_ROOT}/LICENSE.txt"
                "${destination}/Intel/Licenses/Intel-XeSS-license.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MFG_DLSS_INTEL_XESS_ROOT}/third-party-programs.txt"
                "${destination}/Intel/Licenses/Intel-XeSS-third-party.txt"
        VERBATIM
    )
endfunction()

set(plugin_sources
    "${CMAKE_CURRENT_BINARY_DIR}/generated/UFGU.rc"
    Pch.hpp
    config/Settings.cpp
    config/Settings.hpp
    diagnostics/FrameTimeSeries.cpp
    diagnostics/FrameTimeSeries.hpp
    enb/EnbApi.cpp
    enb/EnbApi.hpp
    plugin/Plugin.cpp
    providers/FsrUpscaler.cpp
    providers/FsrUpscaler.hpp
    providers/LowLatency.hpp
    providers/LowLatencyController.cpp
    providers/LowLatencyController.hpp
    providers/ModuleIdentity.cpp
    providers/ModuleIdentity.hpp
    providers/ProviderTypes.cpp
    providers/ProviderTypes.hpp
    providers/RuntimeLoader.cpp
    providers/RuntimeLoader.hpp
    providers/RuntimePath.hpp
    providers/RuntimeSignature.cpp
    providers/RuntimeSignature.hpp
    providers/XessUpscaler.cpp
    providers/XessUpscaler.hpp
    render/FsrFrameGeneration.cpp
    render/FsrFrameGeneration.hpp
    render/XessFrameGeneration.cpp
    render/XessFrameGeneration.hpp
    render/ApplyOutcome.hpp
    render/BaseFrameLimiter.cpp
    render/BaseFrameLimiter.hpp
    render/BaseFrameLimiterPolicy.hpp
    render/CameraData.cpp
    render/CameraData.hpp
    render/CameraReprojection.hpp
    render/D3D12Backend.cpp
    render/D3D12Backend.hpp
    render/ComputeDispatchPlan.hpp
    render/MenuBarLayout.hpp
    render/MultiplierProjection.hpp
    render/DisocclusionFillPlan.hpp
    render/NFramePresentationPlan.hpp
    render/NFramePresentationState.hpp
    audio/MenuToneShape.hpp
    audio/MenuAudio.hpp
    audio/MenuAudio.cpp
    render/MotionVectorCensus.cpp
    render/MotionVectorCensusEntry.cpp
    render/MotionVectorCensus.hpp
    render/SamplerMipBias.cpp
    render/SamplerMipBias.hpp
    render/ComputePass.cpp
    render/ComputePass.hpp
    render/DebugViewCore.cpp
    render/DebugViewPass.cpp
    render/DepthContract.hpp
    render/DepthDiagnostics.cpp
    render/DepthDiagnostics.hpp
    render/DepthRestoration.cpp
    render/DepthRestoration.hpp
    render/FrameGenerationChoicePolicy.hpp
    render/FramePacingRules.hpp
    render/JitterContract.hpp
    render/JitterInjection.hpp
    render/JitterOwnership.hpp
    render/HookPreflight.hpp
    render/RuntimeCompatibility.hpp
    render/RuntimeCompatibilitySkse.hpp
    render/MainDepthBindingPolicy.hpp
    render/MainDepthTracker.cpp
    render/MainDepthTracker.hpp
    render/OutputCapControls.hpp
    render/ResourceProbe.cpp
    render/ResourceProbe.hpp
    render/DynamicResolution.cpp
    render/DynamicResolution.hpp
    render/PresentationBridge.cpp
    render/ProfileChangeController.cpp
    render/PresentationBridge.hpp
    render/RenderDebug.cpp
    render/RenderDebug.hpp
    render/RendererDiagnostics.cpp
    render/RendererDiagnostics.hpp
    render/RendererRuntime.cpp
    render/RendererRuntime.hpp
    render/ScaleformBoundary.cpp
    render/ScaleformBoundary.hpp
    render/SharpeningPass.cpp
    render/SharpeningPass.hpp
    render/SharedResources.cpp
    render/SharedResources.hpp
    render/SharedTextureDescription.hpp
    render/StatusOverlay.cpp
    render/StatusOverlay.hpp
    render/SurfaceBlit.cpp
    render/SurfaceBlit.hpp
    render/UiCompositePass.cpp
    render/UiCompositePass.hpp
    render/UpscalingPass.cpp
    render/UpscalingPass.hpp
    streamline/FeatureSupport.cpp
    streamline/FeatureSupport.hpp
    streamline/FrameGeneration.cpp
    streamline/FrameGeneration.hpp
    streamline/FrameGenerationRecoveryPolicy.hpp
    streamline/FrameSubmission.cpp
    streamline/FrameSubmission.hpp
    streamline/StreamlineApi.cpp
    streamline/StreamlineApi.hpp
    streamline/SuperResolution.cpp
    streamline/SuperResolution.hpp
    streamline/TemporalInputPolicy.hpp
)

add_commonlibsse_plugin(
    MFG_DLSS
    NAME "UFGU"
    AUTHOR "veloha"
    VERSION "${PROJECT_VERSION}"
    MINIMUM_SKSE_VERSION 2.0.20
    COMPATIBLE_RUNTIMES ${UFGU_COMPATIBLE_RUNTIMES}
    SOURCES ${plugin_sources}
)

if(MSVC)
    set(
        mfgdlss_external_pathmaps
        "/pathmap:${COMMONLIBSSE_NG_ROOT}=external/CommonLibSSE-NG"
        "/pathmap:${STREAMLINE_SDK_ROOT}=external/Streamline"
        "/pathmap:${MFG_DLSS_AMD_FIDELITYFX_ROOT}=external/AMD-FidelityFX"
        "/pathmap:${MFG_DLSS_AMD_ANTILAG_ROOT}=external/AMD-AntiLag2"
        "/pathmap:${MFG_DLSS_INTEL_XESS_ROOT}=external/Intel-XeSS"
    )
    target_compile_options(MFG_DLSS PRIVATE ${mfgdlss_external_pathmaps})
    if(TARGET CommonLibSSE)
        target_compile_options(CommonLibSSE PRIVATE ${mfgdlss_external_pathmaps})
    endif()
endif()

mfgdlss_stage_vendor_runtimes(
    MFG_DLSS
    "$<TARGET_FILE_DIR:MFG_DLSS>/UFGU"
)

set(mfgdlss_public_package_dir "${MFG_DLSS_PUBLIC_PACKAGE_DIR}")
cmake_path(
    ABSOLUTE_PATH mfgdlss_public_package_dir
    BASE_DIRECTORY "${CMAKE_BINARY_DIR}"
    NORMALIZE
)
set(mfgdlss_binary_dir "${CMAKE_BINARY_DIR}")
cmake_path(NORMAL_PATH mfgdlss_binary_dir)
cmake_path(
    IS_PREFIX mfgdlss_binary_dir
    "${mfgdlss_public_package_dir}"
    NORMALIZE
    mfgdlss_package_is_in_build_tree
)
if(
    NOT mfgdlss_package_is_in_build_tree
    OR mfgdlss_public_package_dir STREQUAL mfgdlss_binary_dir
)
    message(
        FATAL_ERROR
        "MFG_DLSS_PUBLIC_PACKAGE_DIR must be a child directory of the build tree: "
        "${mfgdlss_binary_dir}"
    )
endif()

if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(MFG_DLSS_PUBLIC_RELEASE_BUILD)
        message(
            FATAL_ERROR
            "Public packaging requires vcpkg manifest metadata so dependency notices can be staged."
        )
    endif()
endif()

set(
    mfgdlss_public_notice_mappings
    "${COMMONLIBSSE_NG_ROOT}/LICENSE|CommonLibSSE-NG.txt"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/fmt/copyright|fmt.txt"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/spdlog/copyright|spdlog.txt"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/rapidcsv/copyright|rapidcsv.txt"
    "${MFG_DLSS_AMD_ANTILAG_ROOT}/LICENSE.txt|AMD-Anti-Lag-2.txt"
    "${MFG_DLSS_PROJECT_LICENSE_FILE}|PROJECT-LICENSE.txt"
)
set(
    mfgdlss_public_static_inputs
    "${PROJECT_SOURCE_DIR}/config/UFGU.ini"
    "${PROJECT_SOURCE_DIR}/fomod/info.xml"
    "${PROJECT_SOURCE_DIR}/fomod/ModuleConfig.xml"
    "${PROJECT_SOURCE_DIR}/fomod/images/header.png"
)
foreach(notice_mapping IN LISTS mfgdlss_public_notice_mappings)
    string(REPLACE "|" ";" notice_parts "${notice_mapping}")
    list(GET notice_parts 0 notice_source)
    list(APPEND mfgdlss_public_static_inputs "${notice_source}")
endforeach()

if(MFG_DLSS_PUBLIC_RELEASE_BUILD)
    set(mfgdlss_missing_public_inputs "")
    foreach(input_file IN LISTS mfgdlss_public_static_inputs)
        if(NOT EXISTS "${input_file}" OR IS_DIRECTORY "${input_file}")
            list(APPEND mfgdlss_missing_public_inputs "${input_file}")
        else()
            file(SIZE "${input_file}" input_size)
            if(input_size EQUAL 0)
                list(APPEND mfgdlss_missing_public_inputs "${input_file} (empty)")
            endif()
        endif()
    endforeach()
    if(mfgdlss_missing_public_inputs)
        message(
            FATAL_ERROR
            "Public packaging is missing required non-empty metadata/notices: "
            "${mfgdlss_missing_public_inputs}"
        )
    endif()
endif()

set(
    mfgdlss_public_package_plugin_dir
    "${mfgdlss_public_package_dir}/SKSE/Plugins"
)
if(MFG_DLSS_PUBLIC_RELEASE_BUILD)
    add_custom_target(
        MFG_DLSS_PublicPackage ALL
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
                "${mfgdlss_public_package_dir}"
        VERBATIM
    )
else()
    add_custom_target(
        MFG_DLSS_PublicPackage
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
                "${mfgdlss_public_package_dir}"
        VERBATIM
    )
endif()
add_dependencies(MFG_DLSS_PublicPackage MFG_DLSS)
add_custom_command(
    TARGET MFG_DLSS_PublicPackage
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${mfgdlss_public_package_plugin_dir}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${mfgdlss_public_package_dir}/fomod"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${mfgdlss_public_package_dir}/fomod/images"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${mfgdlss_public_package_dir}/Licenses"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${mfgdlss_public_package_dir}/presets/nvidia"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${mfgdlss_public_package_dir}/presets/amd"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${mfgdlss_public_package_dir}/presets/intel"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:MFG_DLSS>"
            "${mfgdlss_public_package_plugin_dir}/UFGU.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/config/UFGU.ini"
            "${mfgdlss_public_package_plugin_dir}/UFGU.ini"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/fomod/info.xml"
            "${mfgdlss_public_package_dir}/fomod/info.xml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/fomod/ModuleConfig.xml"
            "${mfgdlss_public_package_dir}/fomod/ModuleConfig.xml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/fomod/images/header.png"
            "${mfgdlss_public_package_dir}/fomod/images/header.png"
    COMMAND "${CMAKE_COMMAND}"
            "-DMFG_DLSS_PRESET_INPUT=${PROJECT_SOURCE_DIR}/config/UFGU.ini"
            "-DMFG_DLSS_PRESET_OUTPUT=${mfgdlss_public_package_dir}/presets/nvidia/UFGU.ini"
            "-DMFG_DLSS_PRESET_PROVIDER=NVIDIA"
            "-DMFG_DLSS_PRESET_FRAME_GENERATOR=Off"
            -P "${PROJECT_SOURCE_DIR}/cmake/GenerateProviderPreset.cmake"
    COMMAND "${CMAKE_COMMAND}"
            "-DMFG_DLSS_PRESET_INPUT=${PROJECT_SOURCE_DIR}/config/UFGU.ini"
            "-DMFG_DLSS_PRESET_OUTPUT=${mfgdlss_public_package_dir}/presets/amd/UFGU.ini"
            "-DMFG_DLSS_PRESET_PROVIDER=AMD"
            "-DMFG_DLSS_PRESET_FRAME_GENERATOR=AMD"
            -P "${PROJECT_SOURCE_DIR}/cmake/GenerateProviderPreset.cmake"
    COMMAND "${CMAKE_COMMAND}"
            "-DMFG_DLSS_PRESET_INPUT=${PROJECT_SOURCE_DIR}/config/UFGU.ini"
            "-DMFG_DLSS_PRESET_OUTPUT=${mfgdlss_public_package_dir}/presets/intel/UFGU.ini"
            "-DMFG_DLSS_PRESET_PROVIDER=Intel"
            "-DMFG_DLSS_PRESET_FRAME_GENERATOR=Intel"
            -P "${PROJECT_SOURCE_DIR}/cmake/GenerateProviderPreset.cmake"
    VERBATIM
)
foreach(notice_mapping IN LISTS mfgdlss_public_notice_mappings)
    string(REPLACE "|" ";" notice_parts "${notice_mapping}")
    list(GET notice_parts 0 notice_source)
    list(GET notice_parts 1 notice_name)
    add_custom_command(
        TARGET MFG_DLSS_PublicPackage
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${notice_source}"
                "${mfgdlss_public_package_dir}/Licenses/${notice_name}"
        VERBATIM
    )
endforeach()
mfgdlss_copy_required_streamline_runtime(
    MFG_DLSS_PublicPackage
    "${mfgdlss_public_package_plugin_dir}/UFGU/Streamline"
)
mfgdlss_stage_vendor_runtimes(
    MFG_DLSS_PublicPackage
    "${mfgdlss_public_package_plugin_dir}/UFGU"
)
add_custom_command(
    TARGET MFG_DLSS_PublicPackage
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DMFG_DLSS_PACKAGE_DIR=${mfgdlss_public_package_dir}"
            -P "${PROJECT_SOURCE_DIR}/cmake/ValidatePublicPackage.cmake"
    COMMAND "${CMAKE_COMMAND}"
            "-DMFG_DLSS_PACKAGE_DIR=${mfgdlss_public_package_dir}"
            "-DMFG_DLSS_HASH_OUTPUT=${mfgdlss_public_package_dir}-SHA256.txt"
            -P "${PROJECT_SOURCE_DIR}/cmake/WritePackageHashes.cmake"
    VERBATIM
)

target_include_directories(
    MFG_DLSS
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${MFG_DLSS_AMD_FIDELITYFX_ROOT}/Kits/FidelityFX"
        "${MFG_DLSS_INTEL_XESS_ROOT}/inc"
        "${MFG_DLSS_AMD_ANTILAG_ROOT}"
)

target_compile_definitions(
    MFG_DLSS
    PRIVATE
        MFG_DLSS_STREAMLINE_APPLICATION_ID=${MFG_DLSS_STREAMLINE_APPLICATION_ID}

        NTDDI_VERSION=NTDDI_WIN10
)

if(MSVC)
    target_link_options(
        MFG_DLSS
        PRIVATE
            "/PDBALTPATH:%_PDB%"
    )
endif()

if(MSVC AND MFG_DLSS_ENABLE_MSVC_CODE_ANALYSIS)
    target_compile_options(
        MFG_DLSS
        PRIVATE
            /analyze
            /analyze:WX-
    )
endif()

target_precompile_headers(
    MFG_DLSS
    PRIVATE
        Pch.hpp
)

target_link_libraries(
    MFG_DLSS
    PRIVATE
        NVIDIA::NGX
        Psapi.lib
        Streamline::Headers
        d2d1.lib
        d3d11.lib
        d3d12.lib
        winmm.lib
        d3dcompiler.lib
        dwrite.lib
        dxgi.lib
        dxguid.lib
        crypt32.lib
        wintrust.lib
)

set_target_properties(
    MFG_DLSS
    PROPERTIES
        OUTPUT_NAME "UFGU"
)

if(BUILD_TESTING)
    function(mfgdlss_add_header_contract_test target_name test_name source)
        add_executable(${target_name} ${source})
        target_include_directories(
            ${target_name}
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        add_test(NAME ${test_name} COMMAND ${target_name})
    endfunction()

    function(mfgdlss_add_d3d12_contract_test target_name test_name)
        add_executable(${target_name} ${ARGN})
        target_include_directories(
            ${target_name}
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_compile_definitions(
            ${target_name}
            PRIVATE
                NTDDI_VERSION=NTDDI_WIN10
        )
        target_precompile_headers(
            ${target_name}
            PRIVATE
                Pch.hpp
        )
        target_link_libraries(
            ${target_name}
            PRIVATE
                CommonLibSSE::CommonLibSSE
                d3d12.lib
        winmm.lib
                d3dcompiler.lib
                dxgi.lib
                dxguid.lib
        )
        add_test(NAME ${test_name} COMMAND ${target_name})
    endfunction()

    function(mfgdlss_add_d3d11_contract_test target_name test_name)
        add_executable(${target_name} ${ARGN})
        target_include_directories(
            ${target_name}
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(
            ${target_name}
            PRIVATE
                d3d11.lib
                d3dcompiler.lib
                dxgi.lib
        )
        add_test(NAME ${test_name} COMMAND ${target_name})
    endfunction()

    add_executable(
        MFG_DLSS_DepthContractTest
        render/DepthContractTest.cpp
    )
    target_include_directories(
        MFG_DLSS_DepthContractTest
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
        NAME MFG_DLSS.DepthContract
        COMMAND MFG_DLSS_DepthContractTest
    )

    add_executable(
        MFG_DLSS_ProfileChangeControllerTest
        render/ProfileChangeController.cpp
        render/ProfileChangeControllerTest.cpp
    )
    target_include_directories(
        MFG_DLSS_ProfileChangeControllerTest
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
        NAME MFG_DLSS.ProfileChangeController
        COMMAND MFG_DLSS_ProfileChangeControllerTest
    )

    add_executable(
        MFG_DLSS_PresentationModePolicyTest
        render/PresentationModePolicyTest.cpp
    )
    target_include_directories(
        MFG_DLSS_PresentationModePolicyTest
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
        NAME MFG_DLSS.PresentationModePolicy
        COMMAND MFG_DLSS_PresentationModePolicyTest
    )

    add_executable(
        MFG_DLSS_RuntimePathTest
        providers/RuntimePathTest.cpp
    )
    target_include_directories(
        MFG_DLSS_RuntimePathTest
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
        NAME MFG_DLSS.RuntimePath
        COMMAND MFG_DLSS_RuntimePathTest
    )

    add_executable(
        MFG_DLSS_ModuleIdentityTest
        providers/ModuleIdentity.cpp
        providers/ModuleIdentityTest.cpp
    )
    target_include_directories(
        MFG_DLSS_ModuleIdentityTest
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        MFG_DLSS_ModuleIdentityTest
        PRIVATE
            Psapi.lib
    )
    add_test(
        NAME MFG_DLSS.ModuleIdentity
        COMMAND MFG_DLSS_ModuleIdentityTest
    )

    add_executable(
        MFG_DLSS_FrameTimeSeriesTest
        diagnostics/FrameTimeSeries.cpp
        diagnostics/FrameTimeSeriesTest.cpp
    )
    target_include_directories(
        MFG_DLSS_FrameTimeSeriesTest
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
        NAME MFG_DLSS.FrameTimeSeries
        COMMAND MFG_DLSS_FrameTimeSeriesTest
    )

    mfgdlss_add_header_contract_test(
        MFG_DLSS_FramePacingRulesTest
        MFG_DLSS.FramePacingRules
        render/FramePacingRulesTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_ComputeDispatchPlanTest
        MFG_DLSS.ComputeDispatchPlan
        render/ComputeDispatchPlanTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_MenuToneShapeTest
        MFG_DLSS.MenuToneShape
        audio/MenuToneShapeTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_NFramePresentationPlanTest
        MFG_DLSS.NFramePresentationPlan
        render/NFramePresentationPlanTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_NFramePresentationStateTest
        MFG_DLSS.NFramePresentationState
        render/NFramePresentationStateTest.cpp
    )
        mfgdlss_add_header_contract_test(
        MFG_DLSS_DisocclusionFillPlanTest
        MFG_DLSS.DisocclusionFillPlan
        render/DisocclusionFillPlanTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_MultiplierProjectionTest
        MFG_DLSS.MultiplierProjection
        render/MultiplierProjectionTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_MenuBarLayoutTest
        MFG_DLSS.MenuBarLayout
        render/MenuBarLayoutTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_SamplerMipBiasTest
        MFG_DLSS.SamplerMipBias
        render/SamplerMipBiasTest.cpp
    )
    mfgdlss_add_d3d12_contract_test(
        MFG_DLSS_MotionVectorCensusContractTest
        MFG_DLSS.MotionVectorCensusContract
        render/MotionVectorCensusContractTest.cpp
        render/MotionVectorCensus.cpp
        render/ComputePass.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_BaseFrameLimiterPolicyTest
        MFG_DLSS.BaseFrameLimiterPolicy
        render/BaseFrameLimiterPolicyTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_FrameGenerationChoicePolicyTest
        MFG_DLSS.FrameGenerationChoicePolicy
        render/FrameGenerationChoicePolicyTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_ProjectIdentityTest
        MFG_DLSS.ProjectIdentity
        streamline/ProjectIdentityTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_ProfilePreflightRulesTest
        MFG_DLSS.ProfilePreflightRules
        render/ProfilePreflightRulesTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_DebugViewIndexTest
        MFG_DLSS.DebugViewIndex
        render/DebugViewIndexTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_MenuGeometryTest
        MFG_DLSS.MenuGeometry
        render/MenuGeometryTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_FrameTagPolicyTest
        MFG_DLSS.FrameTagPolicy
        streamline/FrameTagPolicyTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_FrameGenerationRecoveryPolicyTest
        MFG_DLSS.FrameGenerationRecoveryPolicy
        streamline/FrameGenerationRecoveryPolicyTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_FrameGenerationFramePolicyTest
        MFG_DLSS.FrameGenerationFramePolicy
        streamline/FrameGenerationFramePolicyTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_NativeUiPolicyTest
        MFG_DLSS.NativeUiPolicy
        render/NativeUiPolicyTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_JitterContractTest
        MFG_DLSS.JitterContract
        render/JitterContractTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_CameraReprojectionTest
        MFG_DLSS.CameraReprojection
        render/CameraReprojectionTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_JitterOwnershipTest
        MFG_DLSS.JitterOwnership
        render/JitterOwnershipTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_JitterInjectionTest
        MFG_DLSS.JitterInjection
        render/JitterInjectionTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_RuntimeCompatibilityTest
        MFG_DLSS.RuntimeCompatibility
        render/RuntimeCompatibilityTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_HookPreflightTest
        MFG_DLSS.HookPreflight
        render/HookPreflightTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_MainDepthBindingPolicyTest
        MFG_DLSS.MainDepthBindingPolicy
        render/MainDepthBindingPolicyTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_SharedTextureDescriptionTest
        MFG_DLSS.SharedTextureDescription
        render/SharedTextureDescriptionTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_ApplyOutcomeTest
        MFG_DLSS.ApplyOutcome
        render/ApplyOutcomeTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_LowLatencyTest
        MFG_DLSS.LowLatency
        providers/LowLatencyTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_OutputCapControlsTest
        MFG_DLSS.OutputCapControls
        render/OutputCapControlsTest.cpp
    )
    mfgdlss_add_header_contract_test(
        MFG_DLSS_TemporalInputPolicyTest
        MFG_DLSS.TemporalInputPolicy
        streamline/TemporalInputPolicyTest.cpp
    )

    mfgdlss_add_d3d11_contract_test(
        MFG_DLSS_DepthCopyContractTest
        MFG_DLSS.DepthCopyContract
        render/DepthCopyContractTest.cpp
    )

    mfgdlss_add_d3d11_contract_test(
        MFG_DLSS_SharedUavContractTest
        MFG_DLSS.SharedUavContract
        render/SharedUavContractTest.cpp
    )
    target_link_libraries(
        MFG_DLSS_SharedUavContractTest
        PRIVATE
            d3d12.lib
        winmm.lib
            dxguid.lib
    )

    mfgdlss_add_d3d11_contract_test(
        MFG_DLSS_DebugViewCoreTest
        MFG_DLSS.DebugViewCore
        render/DebugViewCore.cpp
        render/DebugViewCoreTest.cpp
    )
    mfgdlss_add_d3d11_contract_test(
        MFG_DLSS_DebugOffInertTest
        MFG_DLSS.DebugOffInert
        render/DebugViewCore.cpp
        render/DebugOffInertTest.cpp
    )
    mfgdlss_add_d3d11_contract_test(
        MFG_DLSS_ResourceProbeTest
        MFG_DLSS.ResourceProbe
        render/DebugViewCore.cpp
        render/ResourceProbe.cpp
        render/ResourceProbeTest.cpp
    )

    if(MFG_DLSS_PUBLIC_RELEASE_BUILD)
        add_executable(
            MFG_DLSS_PublicConfigurationContractTest
            config/PublicConfigurationContractTest.cpp
        )
        add_dependencies(
            MFG_DLSS_PublicConfigurationContractTest
            MFG_DLSS_PublicPackage
        )
        add_test(
            NAME MFG_DLSS.PublicConfigurationContract
            COMMAND MFG_DLSS_PublicConfigurationContractTest
                "${MFG_DLSS_PUBLIC_PACKAGE_DIR}/SKSE/Plugins/UFGU.ini"
        )

        add_executable(
            MFG_DLSS_ProviderRuntimeContractTest
            providers/ProviderRuntimeContractTest.cpp
            providers/RuntimeSignature.cpp
        )
        target_include_directories(
            MFG_DLSS_ProviderRuntimeContractTest
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
                "${STREAMLINE_SDK_ROOT}/include"
                "${MFG_DLSS_AMD_FIDELITYFX_ROOT}/Kits/FidelityFX"
                "${MFG_DLSS_INTEL_XESS_ROOT}/inc"
        )
        target_link_libraries(
            MFG_DLSS_ProviderRuntimeContractTest
            PRIVATE
                crypt32.lib
                d3d12.lib
        winmm.lib
                wintrust.lib
        )
        add_dependencies(
            MFG_DLSS_ProviderRuntimeContractTest
            MFG_DLSS_PublicPackage
        )
        add_test(
            NAME MFG_DLSS.ProviderRuntimeContract
            COMMAND MFG_DLSS_ProviderRuntimeContractTest
                "${MFG_DLSS_PUBLIC_PACKAGE_DIR}/SKSE/Plugins"
        )

        add_test(
            NAME MFG_DLSS.PublicPackageContents
            COMMAND "${CMAKE_COMMAND}"
                    "-DMFG_DLSS_PACKAGE_DIR=${mfgdlss_public_package_dir}"
                    -P "${PROJECT_SOURCE_DIR}/cmake/ValidatePublicPackage.cmake"
        )
    endif()
endif()

if(DEFINED MFG_DLSS_MO2_MOD_ROOT AND MFG_DLSS_MO2_MOD_ROOT)
    add_custom_command(
        TARGET MFG_DLSS
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${MFG_DLSS_MO2_MOD_ROOT}/SKSE/Plugins"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:MFG_DLSS>"
                "${MFG_DLSS_MO2_MOD_ROOT}/SKSE/Plugins"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${PROJECT_SOURCE_DIR}/config/UFGU.ini"
                "${MFG_DLSS_MO2_MOD_ROOT}/SKSE/Plugins/UFGU.ini"
        VERBATIM
    )

    mfgdlss_copy_required_streamline_runtime(
        MFG_DLSS
        "${MFG_DLSS_MO2_MOD_ROOT}/SKSE/Plugins/UFGU/Streamline"
    )
    mfgdlss_stage_vendor_runtimes(
        MFG_DLSS
        "${MFG_DLSS_MO2_MOD_ROOT}/SKSE/Plugins/UFGU"
    )
endif()
