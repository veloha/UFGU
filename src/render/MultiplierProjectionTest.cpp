#include "render/MultiplierProjection.hpp"

#include <cstdio>
#include <string>

namespace
{
int g_passed = 0;
int g_failed = 0;

void check(const bool condition, const std::string& what)
{
    if (condition) {
        ++g_passed;
        std::printf("  PASS  %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

using namespace mfgdlss::render;

static_assert(
    project_multiplier(60.0F, 6U, 240U).output_fps == 240.0F,
    "6x from a 60 base against a 240 cap delivers 240, not 360. The menu "
    "advertised 360 until 227 because it multiplied and never looked at the "
    "cap");

static_assert(
    project_multiplier(60.0F, 6U, 240U).implied_base_fps == 40.0F,
    "and the base is pulled to 40 to make that cadence, which is the number "
    "that actually decides how the generated frames look");

static_assert(
    project_multiplier(60.0F, 6U, 240U).cap_binds,
    "the projection says plainly that the cap is what decided the result");

static_assert(
    project_multiplier(60.0F, 2U, 240U).output_fps == 120.0F &&
        project_multiplier(60.0F, 2U, 240U).implied_base_fps == 60.0F &&
        !project_multiplier(60.0F, 2U, 240U).cap_binds,
    "2x from the same base is under the cap, so nothing is clamped and the "
    "base is untouched");

static_assert(
    project_multiplier(60.0F, 4U, 240U).output_fps == 240.0F &&
        project_multiplier(60.0F, 4U, 240U).implied_base_fps == 60.0F &&
        !project_multiplier(60.0F, 4U, 240U).cap_binds,
    "4x lands exactly on the cap, which is not the cap binding: the base "
    "still holds at 60 and this is the sweet spot for a 240 cap");

static_assert(
    project_multiplier(60.0F, 6U, 0U).output_fps == 360.0F,
    "with no output cap the multiplier is free again");

static_assert(
    project_multiplier(60.0F, 0U, 240U).output_fps == 60.0F,
    "Off reports the base rate rather than zero");

static_assert(
    project_multiplier(60.0F, 6U, 240U).base_is_thin &&
        !project_multiplier(60.0F, 4U, 240U).base_is_thin,
    "6x against this cap leaves a base too thin to interpolate cleanly while "
    "4x does not, which is exactly the difference veloha reported as ghosting "
    "at 6x");

static_assert(
    !project_multiplier(60.0F, 1U, 60U).base_is_thin,
    "1x is never called thin, because there is nothing being generated");

static_assert(
    projection_is_measurable(60.0F) && !projection_is_measurable(2.0F),
    "a base rate below the floor is not treated as a measurement");

static_assert(
    reference_base_fps(40.0F, 6U, 240U, 60U) == 60.0F,
    "while 6x under a 240 cap holds the base at 40, the reference for "
    "projecting the OTHER multipliers is the configured base cap of 60. "
    "Projecting from 40 was self-referential and made 4x read 161 fps when "
    "switching to it would actually restore the base and give 240");

static_assert(
    reference_base_fps(60.0F, 4U, 240U, 60U) == 60.0F,
    "when the cap is not binding the measured base is already the truth and "
    "is used unchanged");

static_assert(
    reference_base_fps(30.0F, 6U, 240U, 0U) == 30.0F,
    "with no base cap configured there is nothing better to fall back to "
    "than the measurement");

static_assert(
    project_multiplier(reference_base_fps(40.0F, 6U, 240U, 60U), 4U, 240U)
            .output_fps == 240.0F,
    "and with the reference corrected, 4x under a 240 cap projects the 240 it "
    "actually delivers");

static_assert(
    reference_base_fps(30.0F, 6U, 240U, 60U) == 30.0F,
    "a base of 30 at 6x reaches only 180 against a 240 cap, so the CARD is "
    "the limit and not the cap. Falling back to the base cap here would "
    "promise frames the hardware cannot render");
}


static_assert(
    reflex_limit_preserving_base(240U, 60U, 6U) == 360U,
    "MEASURED 2026-08-21 in Riverwood: at 6x with a 240 output cap Streamline "
    "divided the cap by the presented count and paced the base at 40 FPS, "
    "putting rendered frames 25ms apart and stretching five interpolations "
    "across that gap, which is what the ghosting was. Submitting base times "
    "multiplier instead restored a 60 FPS base at 358 displayed and the "
    "ghosting went away entirely");

static_assert(
    reflex_limit_preserving_base(240U, 60U, 4U) == 240U,
    "4x already satisfied base times multiplier, which is exactly why 4x never "
    "showed the fault and 6x always did. The fix must leave the working case "
    "byte for byte unchanged");

static_assert(
    reflex_limit_preserving_base(240U, 40U, 6U) == 240U &&
        reflex_limit_preserving_base(240U, 72U, 6U) == 432U,
    "the limit follows whatever base is configured rather than a fixed rate, "
    "so 6x works at any base instead of only at one the cap happens to divide "
    "into evenly");

static_assert(
    reflex_limit_preserving_base(0U, 60U, 6U) == 0U,
    "an uncapped output stays uncapped. Raising a zero to a number would "
    "install a pacer where the user asked for none");

static_assert(
    reflex_limit_preserving_base(240U, 60U, 1U) == 240U,
    "with generation off the submitted limit is the output limit untouched, "
    "because there is no presented-frame count for Streamline to divide by");

static_assert(
    reflex_limit_preserving_base(600U, 60U, 6U) == 360U,
    "MEASURED 2026-08-21: the cap is base times multiplier EXACTLY, never the "
    "larger of the two. Taking the maximum re-created the two-pacer conflict "
    "from the other direction, because submitting 600 at 6x makes Streamline "
    "pace the base at 100 while our own limiter holds it at 60, and two "
    "pacers disagreeing about one frame rate is worse than either rate alone");

static_assert(
    reflex_limit_preserving_base(240U, 30U, 6U) == 180U,
    "a base the configured cap exceeds still yields base times multiplier, so "
    "the submitted limit and the base limiter always agree");

int main()
{
    std::printf("=== Multiplier projection contract ===\n\n");

    const auto six = project_multiplier(60.0F, 6U, 240U);
    check(
        six.output_fps == 240.0F && six.implied_base_fps == 40.0F,
        "the measured case from 2026-08-20 reproduces exactly: base 60, cap "
        "240, 6x gives 240 out and a 40 base, which is what the log recorded "
        "as median 25.01ms");

    const auto three = project_multiplier(60.0F, 3U, 240U);
    check(
        three.output_fps == 180.0F && !three.cap_binds,
        "3x stays under the cap and keeps its base");

    const auto uncapped = project_multiplier(40.0F, 6U, 0U);
    check(
        uncapped.output_fps == 240.0F && uncapped.implied_base_fps == 40.0F,
        "removing the cap does not invent base rate the card cannot render");

    check(
        project_multiplier(120.0F, 2U, 240U).output_fps == 240.0F &&
            !project_multiplier(120.0F, 2U, 240U).base_is_thin,
        "a high base at a low multiplier fills the cap without going thin, "
        "which is the configuration to steer people toward");

    std::printf(
        "\n=== MultiplierProjectionTest: %d passed, %d failed ===\n",
        g_passed,
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
