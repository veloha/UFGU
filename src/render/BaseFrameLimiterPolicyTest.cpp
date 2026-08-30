#include "render/BaseFrameLimiterPolicy.hpp"

using namespace mfgdlss::render;

int main()
{
    int failures{};
    const auto check = [&failures](const bool condition) {
        failures += condition ? 0 : 1;
    };

    check(!base_frame_limit_active(
        0U, GenerationCadence::fixed_multiplier));
    check(base_frame_limit_active(
        40U, GenerationCadence::fixed_multiplier));
    check(!base_frame_limit_active(
        40U, GenerationCadence::dynamic_to_display_refresh));
    check(!base_frame_limit_active(
        40U, GenerationCadence::dynamic_to_output_cap));
    return failures;
}
