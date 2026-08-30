#include "streamline/FrameGenerationFramePolicy.hpp"

int main()
{
    using mfgdlss::streamline::IncompleteFrameAction;
    using mfgdlss::streamline::incomplete_frame_action;

    int failures = 0;
    const auto check = [&failures](const bool condition) {
        failures += condition ? 0 : 1;
    };

    check(incomplete_frame_action(true, true, true) ==
          IncompleteFrameAction::none);

    check(incomplete_frame_action(false, false, false) ==
          IncompleteFrameAction::publish_inactive_tags_keep_mode);
    check(incomplete_frame_action(true, false, false) ==
          IncompleteFrameAction::publish_inactive_tags_keep_mode);

    check(incomplete_frame_action(true, true, false) ==
          IncompleteFrameAction::suspend_mode);

    return failures;
}
