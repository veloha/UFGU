#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace mfgdlss::audio
{
inline constexpr std::uint32_t kToneSampleRate = 48000U;
inline constexpr std::uint32_t kToneChannels = 1U;

inline constexpr float kGentleLowestHz = 180.0F;
inline constexpr float kGentleHighestHz = 900.0F;
inline constexpr float kGentleLongestSeconds = 0.26F;
inline constexpr float kGentleLoudestGain = 0.22F;
inline constexpr float kGentleSlewPerSample = 0.02F;

struct ToneSpec final
{
    float frequency_hz{};
    float duration_seconds{};
    float gain{};
};

[[nodiscard]] constexpr bool tone_is_gentle(const ToneSpec& tone) noexcept
{
    return tone.frequency_hz >= kGentleLowestHz &&
        tone.frequency_hz <= kGentleHighestHz &&
        tone.duration_seconds > 0.0F &&
        tone.duration_seconds <= kGentleLongestSeconds && tone.gain > 0.0F &&
        tone.gain <= kGentleLoudestGain;
}

[[nodiscard]] constexpr std::size_t tone_sample_count(
    const ToneSpec& tone) noexcept
{
    if (tone.duration_seconds <= 0.0F) {
        return 0U;
    }
    return static_cast<std::size_t>(
        tone.duration_seconds * static_cast<float>(kToneSampleRate));
}

[[nodiscard]] inline float tone_envelope(
    const std::size_t index,
    const std::size_t total) noexcept
{
    if (total == 0U || index >= total) {
        return 0.0F;
    }
    const auto position =
        static_cast<float>(index) / static_cast<float>(total - 1U);
    constexpr auto pi = 3.14159265358979323846F;
    return 0.5F - 0.5F * std::cos(2.0F * pi * position);
}

[[nodiscard]] inline float tone_sample(
    const ToneSpec& tone,
    const std::size_t index) noexcept
{
    const auto total = tone_sample_count(tone);
    if (index >= total) {
        return 0.0F;
    }
    constexpr auto pi = 3.14159265358979323846F;
    const auto seconds =
        static_cast<float>(index) / static_cast<float>(kToneSampleRate);
    const auto carrier =
        std::sin(2.0F * pi * tone.frequency_hz * seconds);
    return carrier * tone_envelope(index, total) * tone.gain;
}

[[nodiscard]] inline std::int16_t tone_pcm(
    const ToneSpec& tone,
    const std::size_t index) noexcept
{
    const auto value = tone_sample(tone, index);
    const auto clamped = value > 1.0F ? 1.0F : (value < -1.0F ? -1.0F : value);
    return static_cast<std::int16_t>(clamped * 32767.0F);
}

inline constexpr ToneSpec kToneMove{392.0F, 0.055F, 0.10F};
inline constexpr ToneSpec kToneSection{294.0F, 0.075F, 0.11F};
inline constexpr ToneSpec kToneValue{523.0F, 0.050F, 0.09F};
inline constexpr ToneSpec kToneApply{440.0F, 0.150F, 0.14F};
inline constexpr ToneSpec kToneOpen{330.0F, 0.130F, 0.12F};
inline constexpr ToneSpec kToneClose{247.0F, 0.130F, 0.12F};
inline constexpr ToneSpec kToneRefused{220.0F, 0.180F, 0.13F};
}
