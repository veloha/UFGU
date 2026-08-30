#include "render/PresentationModePolicy.hpp"

#include <cstdio>

namespace
{
int failures{};

void check(const bool condition, const char* const message)
{
    std::printf("%s %s\n", condition ? "[PASS]" : "[FAIL]", message);
    if (!condition) {
        ++failures;
    }
}
}

int main()
{
    using mfgdlss::config::UpscalingMode;
    using mfgdlss::render::requires_reduced_render_extent;
    using mfgdlss::render::uses_complete_frame_presentation;
    using mfgdlss::render::valid_complete_frame_extent;

    check(
        !uses_complete_frame_presentation(UpscalingMode::off),
        "Off uses the direct presentation route");
    check(
        uses_complete_frame_presentation(UpscalingMode::dlaa),
        "DLAA uses the complete-frame presentation route");
    check(
        !requires_reduced_render_extent(UpscalingMode::dlaa),
        "DLAA keeps the native render extent");
    check(
        valid_complete_frame_extent(
            UpscalingMode::dlaa, 3840, 2160, 3840, 2160),
        "DLAA accepts a native-size complete-frame surface");
    check(
        !valid_complete_frame_extent(
            UpscalingMode::dlaa, 2560, 1440, 3840, 2160),
        "DLAA rejects a reduced render extent");

    for (const auto mode : {
             UpscalingMode::quality,
             UpscalingMode::balanced,
             UpscalingMode::performance,
             UpscalingMode::ultra_performance}) {
        check(
            uses_complete_frame_presentation(mode),
            "Upscaling mode uses the complete-frame presentation route");
        check(
            requires_reduced_render_extent(mode),
            "Upscaling mode requires a reduced render extent");
        check(
            valid_complete_frame_extent(mode, 2560, 1440, 3840, 2160),
            "Upscaling mode accepts a reduced complete-frame surface");
        check(
            !valid_complete_frame_extent(mode, 3840, 2160, 3840, 2160),
            "Upscaling mode rejects a native-size render extent");
    }

    return failures == 0 ? 0 : 1;
}
