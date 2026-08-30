#include "render/MainDepthBindingPolicy.hpp"

using namespace mfgdlss::render;

int main()
{
    int failures{};
    const auto check = [&failures](const bool condition) {
        failures += condition ? 0 : 1;
    };

    const MainDepthCandidate valid{
        3840U, 2160U, 3840U, 2160U, 1U, 1U, true, true};
    check(valid_main_depth_candidate(valid));

    auto candidate = valid;
    candidate.depth_height = 1080U;
    check(!valid_main_depth_candidate(candidate));
    candidate = valid;
    candidate.depth_samples = 4U;
    check(!valid_main_depth_candidate(candidate));
    candidate = valid;
    candidate.depth_array_size = 2U;
    check(!valid_main_depth_candidate(candidate));
    candidate = valid;
    candidate.depth_stencil_bind = false;
    check(!valid_main_depth_candidate(candidate));
    candidate = valid;
    candidate.shader_resource_bind = false;
    check(!valid_main_depth_candidate(candidate));
    candidate = valid;
    candidate.color_width = 0U;
    check(!valid_main_depth_candidate(candidate));

    return failures;
}
