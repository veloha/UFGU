#include "audio/MenuAudio.hpp"

#include "config/Settings.hpp"

#include <Windows.h>

#include <mmsystem.h>

#include <SKSE/SKSE.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mfgdlss::audio
{
namespace
{
namespace logger = SKSE::log;

constexpr std::size_t kCueCount = 7U;
constexpr std::uint32_t kWavHeaderBytes = 44U;

[[nodiscard]] const ToneSpec& tone_for(const MenuCue cue) noexcept
{
    switch (cue) {
    case MenuCue::section: return kToneSection;
    case MenuCue::value: return kToneValue;
    case MenuCue::apply: return kToneApply;
    case MenuCue::open: return kToneOpen;
    case MenuCue::close: return kToneClose;
    case MenuCue::refused: return kToneRefused;
    case MenuCue::move: break;
    }
    return kToneMove;
}

void write_u32(std::vector<std::uint8_t>& wav, const std::uint32_t value)
{
    wav.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    wav.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    wav.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    wav.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void write_u16(std::vector<std::uint8_t>& wav, const std::uint16_t value)
{
    wav.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    wav.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void write_tag(std::vector<std::uint8_t>& wav, const char* const tag)
{
    for (std::size_t index = 0; index < 4U; ++index) {
        wav.push_back(static_cast<std::uint8_t>(tag[index]));
    }
}

[[nodiscard]] std::vector<std::uint8_t> render_wav(
    const ToneSpec& tone,
    const float volume)
{
    const auto total = tone_sample_count(tone);
    const auto payload = static_cast<std::uint32_t>(total * sizeof(std::int16_t));

    std::vector<std::uint8_t> wav;
    wav.reserve(kWavHeaderBytes + payload);
    write_tag(wav, "RIFF");
    write_u32(wav, 36U + payload);
    write_tag(wav, "WAVE");
    write_tag(wav, "fmt ");
    write_u32(wav, 16U);
    write_u16(wav, 1U);
    write_u16(wav, static_cast<std::uint16_t>(kToneChannels));
    write_u32(wav, kToneSampleRate);
    write_u32(wav, kToneSampleRate * kToneChannels * 2U);
    write_u16(wav, static_cast<std::uint16_t>(kToneChannels * 2U));
    write_u16(wav, 16U);
    write_tag(wav, "data");
    write_u32(wav, payload);

    for (std::size_t index = 0; index < total; ++index) {
        const auto scaled = static_cast<std::int16_t>(
            static_cast<float>(tone_pcm(tone, index)) * volume);
        write_u16(wav, static_cast<std::uint16_t>(scaled));
    }
    return wav;
}
}

struct MenuAudio::State final
{
    std::array<std::vector<std::uint8_t>, kCueCount> wavs;
    float rendered_volume{-1.0F};
};

MenuAudio& MenuAudio::instance() noexcept
{
    static MenuAudio audio;
    return audio;
}

bool MenuAudio::available() const noexcept
{
    return state_ != nullptr;
}

bool MenuAudio::ensure_started()
{
    if (state_ == nullptr) {
        state_ = new State{};
        logger::info(
            "Menu audio is ready. Every cue is synthesised at run time from a "
            "sine under a raised-cosine envelope and played from memory, so "
            "nothing ships as an audio file and no cue can begin or end on a "
            "discontinuity. The tones are constrained at compile time to "
            "{}Hz to {}Hz, at most {:.2f}s and at most {:.2f} gain",
            kGentleLowestHz,
            kGentleHighestHz,
            kGentleLongestSeconds,
            kGentleLoudestGain);
    }

    const auto volume = config::Settings::instance().menu_sound_volume();
    if (state_->rendered_volume == volume) {
        return true;
    }

    constexpr std::array<MenuCue, kCueCount> cues{
        MenuCue::move,
        MenuCue::section,
        MenuCue::value,
        MenuCue::apply,
        MenuCue::open,
        MenuCue::close,
        MenuCue::refused};
    for (const auto cue : cues) {
        state_->wavs[static_cast<std::size_t>(cue)] =
            render_wav(tone_for(cue), volume);
    }
    state_->rendered_volume = volume;
    return true;
}

void MenuAudio::play(const MenuCue cue)
{
    auto& settings = config::Settings::instance();
    if (!settings.menu_sounds_enabled()) {
        return;
    }
    if (settings.menu_sound_volume() <= 0.0F) {
        return;
    }
    if (!ensure_started()) {
        return;
    }

    const auto& wav = state_->wavs[static_cast<std::size_t>(cue)];
    if (wav.empty()) {
        return;
    }
    static_cast<void>(PlaySoundW(
        reinterpret_cast<LPCWSTR>(wav.data()),
        nullptr,
        SND_MEMORY | SND_ASYNC | SND_NODEFAULT));
}

void MenuAudio::shutdown() noexcept
{
    if (state_ == nullptr) {
        return;
    }
    static_cast<void>(PlaySoundW(nullptr, nullptr, SND_PURGE));
    delete state_;
    state_ = nullptr;
}
}
