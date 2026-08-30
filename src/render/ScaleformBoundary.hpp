#pragma once

#include <RE/Skyrim.h>

#include <cstdint>

namespace mfgdlss::render
{

class ScaleformBoundary final
{
public:
    [[nodiscard]] static ScaleformBoundary& instance() noexcept;

    [[nodiscard]] bool ensure_installed() noexcept;
    [[nodiscard]] bool installed() const noexcept;
    void begin_frame() noexcept;

    [[nodiscard]] bool ui_phase_opened_this_frame() const noexcept;
    [[nodiscard]] std::uint32_t displays_this_frame() const noexcept;
    [[nodiscard]] bool inside_display() const noexcept;

    void note_first_composite() noexcept;
    void shutdown() noexcept;

private:

    static void begin_display_thunk(
        RE::GRenderer* renderer,
        const RE::GColor* background_color,
        const RE::GViewport& viewport,
        float x0,
        float x1,
        float y0,
        float y1);
    static void end_display_thunk(RE::GRenderer* renderer);

    void before_display() noexcept;

    void after_display(const RE::GViewport& viewport) noexcept;
    void note_display_completed() noexcept;

    using BeginDisplayFunction = void (*)(
        RE::GRenderer*,
        const RE::GColor*,
        const RE::GViewport&,
        float,
        float,
        float,
        float);
    using EndDisplayFunction = void (*)(RE::GRenderer*);
    static inline BeginDisplayFunction original_begin_display_{};
    static inline EndDisplayFunction original_end_display_{};

    std::uint32_t displays_this_frame_{};
    std::uint32_t install_attempts_{};
    std::uint32_t boundary_reports_{};
    bool installed_{};
    bool abi_confirmed_{};

    bool capture_pending_{};
    bool inside_display_{};
    bool ui_phase_opened_this_frame_{};
    bool renderer_absent_logged_{};
    bool invalid_vtable_logged_{};
    bool vtable_changed_logged_{};
    bool hudless_preserve_failure_logged_{};
    bool scene_resolve_incomplete_logged_{};
    bool full_resolution_capture_start_logged_{};
    bool reduced_capture_start_logged_{};
    bool capture_ended_early_logged_{};
    bool stage_before_logged_{};
    bool stage_after_logged_{};
    bool stage_bound_logged_{};
    bool stage_display_completed_logged_{};
    bool stage_composite_logged_{};
};
}
