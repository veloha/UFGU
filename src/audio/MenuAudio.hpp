#pragma once

#include "audio/MenuToneShape.hpp"

namespace mfgdlss::audio
{
enum class MenuCue
{
    move,
    section,
    value,
    apply,
    open,
    close,
    refused
};

class MenuAudio final
{
public:
    [[nodiscard]] static MenuAudio& instance() noexcept;

    void play(MenuCue cue);
    void shutdown() noexcept;

    [[nodiscard]] bool available() const noexcept;

private:
    MenuAudio() = default;
    ~MenuAudio() = default;
    MenuAudio(const MenuAudio&) = delete;
    MenuAudio& operator=(const MenuAudio&) = delete;

    [[nodiscard]] bool ensure_started();

    struct State;
    State* state_{};
    bool start_failed_{};
};
}
