#pragma once

#include "config/Settings.hpp"
#include "render/FramePacingRules.hpp"
#include "streamline/FrameGenerationRecoveryPolicy.hpp"

#include <cstdint>

namespace sl
{
struct FrameToken;
}

namespace mfgdlss::streamline
{
enum class FrameGenerationGateReason : std::uint32_t
{
    plugin_menu,
    game_paused,
    plugin_menu_and_game_paused
};

class FrameGeneration final
{
public:
    [[nodiscard]] static FrameGeneration& instance() noexcept;

    [[nodiscard]] bool initialize(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t back_buffer_count);

    [[nodiscard]] bool enable(std::uint32_t multiplier);
    [[nodiscard]] bool set_reflex_mode(config::ReflexMode mode);

    [[nodiscard]] bool set_output_target_fps(std::uint32_t output_fps);

    void note_display_refresh(std::uint32_t refresh_hz) noexcept;

    [[nodiscard]] bool suspend(const char* reason);

    [[nodiscard]] bool suspend_for_gate(
        std::uint32_t desired_multiplier,
        FrameGenerationGateReason reason);
    [[nodiscard]] bool resume_from_gate(std::uint32_t desired_multiplier);
    [[nodiscard]] bool gate_suspended() const noexcept;
    [[nodiscard]] bool generation_suspended() const noexcept;
    [[nodiscard]] std::uint64_t suspended_frames() const noexcept;
    [[nodiscard]] const char* suspension_cause_name() const noexcept;

    [[nodiscard]] bool suspend_for_reset(const char* reason);

    void note_presentation_healthy();

    [[nodiscard]] bool resume_if_eligible();
    [[nodiscard]] bool reconfigure(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t back_buffer_count) noexcept;
    void after_present();
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] std::uint32_t multiplier() const noexcept;
    [[nodiscard]] std::uint32_t actual_presents() const noexcept;
    [[nodiscard]] std::uint64_t presented_frames_total() const noexcept;
    [[nodiscard]] bool verified() const noexcept;

    [[nodiscard]] std::uint32_t output_target_fps() const noexcept;
    [[nodiscard]] std::uint32_t applied_render_cap_fps() const noexcept;
    [[nodiscard]] std::uint32_t effective_output_target_fps() const noexcept;
    [[nodiscard]] std::uint32_t measured_multiplier() const noexcept;
    [[nodiscard]] std::uint32_t display_refresh_hz() const noexcept;
    [[nodiscard]] std::uint32_t generation_transitions() const noexcept;
    [[nodiscard]] render::PacingReason pacing_reason() const noexcept;
    [[nodiscard]] bool pacing_target_achievable() const noexcept;
    [[nodiscard]] bool vrr_headroom_applied() const noexcept;

    [[nodiscard]] render::GenerationCadence cadence() const noexcept;

private:
    [[nodiscard]] bool suspend_impl(
        bool retain_resources,
        FrameGenerationSuspensionCause cause);
    [[nodiscard]] static const char* gate_reason_name(
        FrameGenerationGateReason reason) noexcept;
    [[nodiscard]] std::uint32_t effective_desired_multiplier() const noexcept;

    void report_persistent_suspension(FrameGenerationSuspensionCause cause);
    void report_idle_generation();

    [[nodiscard]] std::uint32_t reflex_limit_divisor() const noexcept;

    [[nodiscard]] bool apply_reflex_options(
        config::ReflexMode mode,
        std::uint32_t presentation_frame_limit,
        std::uint32_t generated_multiplier);

    [[nodiscard]] config::ReflexMode effective_reflex_mode(
        config::ReflexMode selected) const noexcept;

    void* set_reflex_options_{};
    void* get_reflex_state_{};
    void* set_dlssg_options_{};
    void* get_dlssg_state_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::uint32_t back_buffer_count_{};
    std::uint32_t maximum_multiplier_{1};
    std::uint32_t refused_multiplier_{};
    std::uint32_t multiplier_{1};
    std::uint32_t temporal_width_{};
    std::uint32_t temporal_height_{};
    std::uint32_t presents_since_state_query_{};
    std::uint32_t unsuccessful_state_queries_{};
    std::uint32_t waiting_present_count_{};
    std::uint32_t actual_presents_{1};
    std::uint64_t presented_frames_total_{};

    std::uint32_t frame_limit_{};
    std::uint32_t reflex_effective_limit_{};

    std::uint32_t output_target_fps_{};
    std::uint32_t display_refresh_hz_{};
    render::MultiplierTracker multiplier_tracker_{};
    render::PacingDecision pacing_decision_{};

    render::CadenceDecision cadence_decision_{};
    std::uint64_t suspended_frames_{};
    std::uint32_t gate_grace_frames_{};
    std::uint64_t idle_frames_{};
    const char* suspension_reason_{"an unrecorded reason"};
    config::ReflexMode reflex_mode_{config::ReflexMode::low_latency};

    FrameGenerationRecoveryPolicy recovery_policy_{};
    FrameGenerationGateReason gate_reason_{
        FrameGenerationGateReason::plugin_menu};

    bool ready_{};
    bool reflex_configured_{};

    config::ReflexMode reflex_submitted_{config::ReflexMode::off};
    bool reflex_submitted_valid_{};
    bool reflex_raise_logged_{};
    bool ui_recomposition_enabled_{};
    bool resources_retained_{};
    bool generated_frames_verified_{};
    bool vendor_generator_logged_{};
    bool vendor_fallback_logged_{};

    bool pacing_refresh_dirty_{};

    bool dynamic_unavailable_logged_{};
};
}
