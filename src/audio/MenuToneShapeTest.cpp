#include "audio/MenuToneShape.hpp"

#include <cmath>
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

using namespace mfgdlss::audio;

static_assert(
    tone_is_gentle(kToneMove) && tone_is_gentle(kToneSection) &&
        tone_is_gentle(kToneValue) && tone_is_gentle(kToneApply) &&
        tone_is_gentle(kToneOpen) && tone_is_gentle(kToneClose) &&
        tone_is_gentle(kToneRefused),
    "every tone this menu can make is inside the calm envelope veloha asked "
    "for, checked at compile time so a later edit cannot quietly add a "
    "piercing one");

static_assert(
    !tone_is_gentle(ToneSpec{4000.0F, 0.05F, 0.10F}),
    "a piercing frequency is refused");
static_assert(
    !tone_is_gentle(ToneSpec{440.0F, 2.0F, 0.10F}),
    "a tone that outlasts the keypress is refused");
static_assert(
    !tone_is_gentle(ToneSpec{440.0F, 0.05F, 0.9F}),
    "a loud tone is refused, because a settings menu should never be the "
    "loudest thing in the room");

static_assert(
    tone_sample_count(kToneApply) == 7200U,
    "150ms at 48kHz is 7200 samples");
static_assert(
    tone_sample_count(ToneSpec{440.0F, 0.0F, 0.1F}) == 0U,
    "a zero length tone produces no samples rather than a divide by zero");

[[nodiscard]] bool peak_within_gain(const ToneSpec& tone)
{
    const auto total = tone_sample_count(tone);
    for (std::size_t index = 0; index < total; ++index) {
        if (std::fabs(tone_sample(tone, index)) > tone.gain + 1.0e-5F) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool never_slews_harshly(const ToneSpec& tone)
{
    const auto total = tone_sample_count(tone);
    auto previous = tone_sample(tone, 0);
    for (std::size_t index = 1; index < total; ++index) {
        const auto current = tone_sample(tone, index);
        if (std::fabs(current - previous) > kGentleSlewPerSample) {
            return false;
        }
        previous = current;
    }
    return true;
}
}

int main()
{
    std::printf("=== Menu tone shape contract ===\n\n");

    check(
        tone_envelope(0U, 7200U) == 0.0F,
        "the envelope starts at exactly silence, which is what stops the "
        "click a raw sine would make at its first sample");
    check(
        tone_envelope(7199U, 7200U) < 1.0e-6F,
        "and it ends at silence too, so releasing a key does not pop");
    check(
        tone_envelope(3600U, 7200U) > 0.99F,
        "the envelope reaches full amplitude in the middle rather than "
        "flattening the tone to nothing");

    check(
        tone_sample(kToneApply, 0U) == 0.0F,
        "the first sample of a tone is silence");
    check(
        tone_sample(kToneApply, tone_sample_count(kToneApply)) == 0.0F,
        "reading past the end returns silence rather than running off the "
        "buffer");

    check(
        peak_within_gain(kToneApply) && peak_within_gain(kToneMove),
        "no sample ever exceeds the tone's own gain, so the mix cannot clip");

    check(
        never_slews_harshly(kToneMove) && never_slews_harshly(kToneApply) &&
            never_slews_harshly(kToneRefused),
        "consecutive samples never jump far, which is the measurable "
        "difference between a soft tone and a harsh one");

    check(
        tone_pcm(kToneApply, 0U) == 0,
        "the encoded stream also starts at zero, so the click cannot come "
        "back through the conversion to 16 bit");

    check(
        kToneClose.frequency_hz < kToneOpen.frequency_hz,
        "closing falls below opening, so the pair reads as down and up "
        "without needing a label");

    std::printf(
        "\n=== MenuToneShapeTest: %d passed, %d failed ===\n",
        g_passed,
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
