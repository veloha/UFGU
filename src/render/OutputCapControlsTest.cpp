#include "render/OutputCapControls.hpp"

#include <cstdio>

namespace
{
int failures{};

void expect(const bool condition, const char* const message)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", message);
    failures += condition ? 0 : 1;
}
}

int main()
{
    using namespace mfgdlss::render;

    expect(valid_output_cap(0), "uncapped is valid");
    expect(valid_output_cap(20), "minimum custom cap is valid");
    expect(valid_output_cap(237), "arbitrary custom cap is valid");
    expect(valid_output_cap(1000), "maximum custom cap is valid");
    expect(!valid_output_cap(19), "below-minimum custom cap is rejected");
    expect(!valid_output_cap(1001), "above-maximum custom cap is rejected");
    expect(output_cap_is_preset(240), "240 FPS is recognized as a preset");
    expect(!output_cap_is_preset(237), "237 FPS is recognized as custom");
    expect(parse_output_cap_digits("237") == 237u,
           "typed custom cap is parsed exactly");
    expect(parse_output_cap_digits("0") == 0u,
           "typed zero selects Off");
    expect(!parse_output_cap_digits(""),
           "an empty custom field is rejected");
    expect(!parse_output_cap_digits("19"),
           "typed value below the supported range is rejected");
    expect(!parse_output_cap_digits("1001"),
           "typed value above the supported range is rejected");
    expect(!parse_output_cap_digits("24x"),
           "non-digits are rejected");
    expect(adjust_output_cap(237, 1) == 240,
           "normal right selects the next preset");
    expect(adjust_output_cap(237, -1) == 165,
           "normal left selects the previous preset");
    expect(adjust_output_cap(240, 1, 1) == 241,
           "fine right supports a one-FPS custom adjustment");
    expect(adjust_output_cap(240, -1, 10) == 230,
           "coarse left supports a ten-FPS custom adjustment");
    expect(adjust_output_cap(0, 1, 1) == 20,
           "custom adjustment enters the supported range from Off");
    expect(adjust_output_cap(20, -1, 1) == 0,
           "custom adjustment can return to Off");
    expect(adjust_output_cap(1000, 1, 10) == 1000,
           "custom adjustment clamps at the maximum");

    static_assert(
        base_for_output_preset(360U, 6U) == 60U,
        "360 at 6x is a 60 base");
    static_assert(
        base_for_output_preset(240U, 6U) == 40U,
        "240 at 6x is a 40 base");
    static_assert(
        base_for_output_preset(165U, 6U) == 0U,
        "165 does not divide by 6 so it is not offered");
    static_assert(
        base_for_output_preset(60U, 6U) == 0U,
        "60 at 6x would need a 10 base, below the 20 minimum");
    static_assert(
        adjust_base_by_output(60U, 6U, 1) == 80U,
        "stepping up from 360 output at 6x reaches 480, an 80 base");
    static_assert(
        adjust_base_by_output(60U, 6U, -1) == 40U,
        "stepping down from 360 output at 6x reaches 240, a 40 base");
    static_assert(
        adjust_base_by_output(60U, 1U, 1) == 60U,
        "at 1x the base is not driven by the output row");

    std::printf(
        "=== OutputCapControlsTest: %d failed ===\n",
        failures);
    return failures == 0 ? 0 : 1;
}
