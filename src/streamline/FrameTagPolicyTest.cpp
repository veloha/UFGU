#include "streamline/FrameTagPolicy.hpp"

int main()
{
    using mfgdlss::streamline::make_frame_tag_policy;
    int failures = 0;
    const auto check = [&failures](const bool condition) {
        failures += condition ? 0 : 1;
    };

    const auto reduced_matches_native =
        make_frame_tag_policy(false, true).submission_allowed ==
        make_frame_tag_policy(false, false).submission_allowed;
    check(reduced_matches_native);

    const auto native_fallback = make_frame_tag_policy(false, false);
    check(native_fallback.submission_allowed);
    check(!native_fallback.use_hudless_ui);
    check(native_fallback.clear_optional_color_tags);
    check(native_fallback.tag_backbuffer_extent);
    check(native_fallback.tag_count == 5);

    const auto native_recomposition = make_frame_tag_policy(true, false);
    check(native_recomposition.submission_allowed);
    check(native_recomposition.use_hudless_ui);
    check(!native_recomposition.clear_optional_color_tags);
    check(native_recomposition.tag_backbuffer_extent);
    check(native_recomposition.tag_count == 5);

    const auto reduced_recomposition = make_frame_tag_policy(true, true);
    check(reduced_recomposition.submission_allowed);
    check(reduced_recomposition.use_hudless_ui);
    check(reduced_recomposition.tag_backbuffer_extent);
    check(reduced_recomposition.tag_count == 5);

    const auto reduced_without_separation = make_frame_tag_policy(false, true);
    check(reduced_without_separation.submission_allowed);
    check(!reduced_without_separation.use_hudless_ui);
    check(reduced_without_separation.clear_optional_color_tags);
    check(reduced_without_separation.tag_backbuffer_extent);
    check(reduced_without_separation.tag_count == 5);

    return failures;
}
