#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mfgdlss::streamline
{
inline constexpr std::string_view kProjectId =
    "d514d00c-85c7-4d05-9516-53b6528d9a6b";
inline constexpr std::string_view kEngineName = "SkyrimSE";

[[nodiscard]] inline std::string engine_version_string(
    const std::uint16_t major,
    const std::uint16_t minor,
    const std::uint16_t patch,
    const std::uint16_t build,
    const std::string_view project_version)
{
    std::string text{kEngineName};
    text += '-';
    text += std::to_string(major);
    text += '.';
    text += std::to_string(minor);
    text += '.';
    text += std::to_string(patch);
    if (build != 0) {
        text += '.';
        text += std::to_string(build);
    }
    text += " UFGU-";
    text += project_version;
    return text;
}
}
