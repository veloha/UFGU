cmake_minimum_required(VERSION 3.25)

foreach(required_variable IN ITEMS
    MFG_DLSS_PRESET_INPUT
    MFG_DLSS_PRESET_OUTPUT
    MFG_DLSS_PRESET_PROVIDER
    MFG_DLSS_PRESET_FRAME_GENERATOR
)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Set ${required_variable} when generating a provider preset.")
    endif()
endforeach()

if(NOT EXISTS "${MFG_DLSS_PRESET_INPUT}")
    message(FATAL_ERROR "Preset input does not exist: ${MFG_DLSS_PRESET_INPUT}")
endif()

file(READ "${MFG_DLSS_PRESET_INPUT}" preset_content)
string(FIND "${preset_content}" "VendorFrameGeneration=Off" generator_marker)
string(FIND "${preset_content}" "Provider=NVIDIA" provider_marker)
if(generator_marker EQUAL -1 OR provider_marker EQUAL -1)
    message(FATAL_ERROR "Preset input is missing the canonical provider markers.")
endif()

string(
    REPLACE
    "VendorFrameGeneration=Off"
    "VendorFrameGeneration=${MFG_DLSS_PRESET_FRAME_GENERATOR}"
    preset_content
    "${preset_content}"
)
string(
    REPLACE
    "Provider=NVIDIA"
    "Provider=${MFG_DLSS_PRESET_PROVIDER}"
    preset_content
    "${preset_content}"
)

cmake_path(GET MFG_DLSS_PRESET_OUTPUT PARENT_PATH preset_output_directory)
file(MAKE_DIRECTORY "${preset_output_directory}")
file(WRITE "${MFG_DLSS_PRESET_OUTPUT}" "${preset_content}")
