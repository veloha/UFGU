#include "render/ProfilePreflightRules.hpp"

#include <string_view>

using namespace mfgdlss::render;

namespace
{
constexpr ProfilePreflightInputs accepted_baseline() noexcept
{
    ProfilePreflightInputs in{};
    in.bridge_ready = true;
    in.output_width = 3840;
    in.output_height = 2160;
    in.renderer_available = true;
    in.extent_resolved = true;
    in.requested_width = 2560;
    in.requested_height = 1440;
    in.current_width = 2560;
    in.current_height = 1440;
    in.stable_proxy_identity_published = false;
    in.live_extent_reconfiguration_supported = false;
    in.current_complete_frame_route = true;
    in.requested_complete_frame_route = true;
    return in;
}
}

int main()
{
    int failures{};
    const auto check = [&failures](const bool condition) {
        failures += condition ? 0 : 1;
    };
    using Decision = ProfilePreflightDecision;

    check(evaluate_profile_preflight(accepted_baseline()) ==
        Decision::accepted);

    {
        auto in = accepted_baseline();
        in.stable_proxy_identity_published = true;
        check(evaluate_profile_preflight(in) == Decision::accepted);
    }

    {
        auto in = accepted_baseline();
        in.bridge_ready = false;
        check(evaluate_profile_preflight(in) ==
            Decision::rejected_no_output_extent);
        in = accepted_baseline();
        in.output_width = 0;
        check(evaluate_profile_preflight(in) ==
            Decision::rejected_no_output_extent);
        in = accepted_baseline();
        in.output_height = 0;
        check(evaluate_profile_preflight(in) ==
            Decision::rejected_no_output_extent);
    }

    {
        auto in = accepted_baseline();
        in.renderer_available = false;
        check(evaluate_profile_preflight(in) == Decision::rejected_no_renderer);
    }

    {
        auto in = accepted_baseline();
        in.extent_resolved = false;
        check(evaluate_profile_preflight(in) ==
            Decision::rejected_extent_unresolved);
    }

    {
        auto in = accepted_baseline();
        in.requested_width = 0;
        check(evaluate_profile_preflight(in) ==
            Decision::rejected_extent_unusable);
        in = accepted_baseline();
        in.requested_width = in.output_width + 1;
        check(evaluate_profile_preflight(in) ==
            Decision::rejected_extent_unusable);
        in = accepted_baseline();
        in.requested_height = in.output_height + 1;
        check(evaluate_profile_preflight(in) ==
            Decision::rejected_extent_unusable);
    }

    {
        auto in = accepted_baseline();
        in.requested_width = in.output_width;
        in.requested_height = in.output_height;
        in.current_width = in.output_width;
        in.current_height = in.output_height;
        check(evaluate_profile_preflight(in) == Decision::accepted);
    }

    {
        auto in = accepted_baseline();
        in.current_width = 0;
        check(evaluate_profile_preflight(in) ==
            Decision::rejected_active_extent_unavailable);
    }

    {
        auto in = accepted_baseline();
        in.requested_complete_frame_route = false;
        check(evaluate_profile_preflight(in) ==
            Decision::rejected_presentation_route_change_requires_restart);
    }

    {
        auto in = accepted_baseline();
        in.requested_complete_frame_route = false;
        in.requested_width = 1920;
        in.stable_proxy_identity_published = true;
        check(evaluate_profile_preflight(in) ==
            Decision::rejected_presentation_route_change_requires_restart);
    }

    {
        auto in = accepted_baseline();
        in.requested_width = 1920;
        in.stable_proxy_identity_published = true;
        check(evaluate_profile_preflight(in) ==
            Decision::rejected_proxy_already_published);
    }

    {
        auto in = accepted_baseline();
        in.requested_width = 1920;
        check(evaluate_profile_preflight(in) ==
            Decision::rejected_live_extent_reconfiguration_unavailable);
        in.live_extent_reconfiguration_supported = true;
        check(evaluate_profile_preflight(in) == Decision::accepted);
    }

    check(!is_rejection(Decision::accepted));
    check(is_rejection(Decision::rejected_no_renderer));
    check(is_rejection(Decision::rejected_proxy_already_published));

    check(!is_user_facing(Decision::accepted));
    check(!is_user_facing(Decision::rejected_no_output_extent));
    check(!is_user_facing(Decision::rejected_no_renderer));
    check(!is_user_facing(Decision::rejected_active_extent_unavailable));
    check(is_user_facing(Decision::rejected_extent_unresolved));
    check(is_user_facing(Decision::rejected_extent_unusable));
    check(is_user_facing(Decision::rejected_proxy_already_published));
    check(is_user_facing(
        Decision::rejected_live_extent_reconfiguration_unavailable));
    check(is_user_facing(
        Decision::rejected_presentation_route_change_requires_restart));

    constexpr Decision kAll[]{
        Decision::accepted,
        Decision::rejected_no_output_extent,
        Decision::rejected_no_renderer,
        Decision::rejected_extent_unresolved,
        Decision::rejected_extent_unusable,
        Decision::rejected_active_extent_unavailable,
        Decision::rejected_proxy_already_published,
        Decision::rejected_live_extent_reconfiguration_unavailable,
        Decision::rejected_presentation_route_change_requires_restart};
    for (const auto decision : kAll) {
        const std::string_view text{describe(decision)};
        check(!text.empty());
        check(text != std::string_view{"unknown"});
    }

    for (const auto decision : kAll) {
        const std::string_view text{describe(decision)};
        const auto announces_restart =
            text.starts_with("Restart required:");
        check(announces_restart ==
            (decision == Decision::rejected_proxy_already_published ||
             decision ==
                 Decision::rejected_live_extent_reconfiguration_unavailable ||
             decision ==
                 Decision::
                     rejected_presentation_route_change_requires_restart));
    }

    static_assert(
        evaluate_profile_preflight(accepted_baseline()) == Decision::accepted,
        "the accepted baseline must stay accepted");

    return failures == 0 ? 0 : 1;
}
