#pragma once

#include "config/Settings.hpp"
#include "providers/ProviderTypes.hpp"

#include <RE/Skyrim.h>

#include <cstdint>
#include <memory>
#include <string>

struct ID3D11Texture2D;

namespace mfgdlss::render
{
class UpscalingPass final
{
public:
    [[nodiscard]] static UpscalingPass& instance() noexcept;

    [[nodiscard]] bool install();
    void shutdown() noexcept;
    [[nodiscard]] bool evaluation_verified() const noexcept;

    [[nodiscard]] ID3D11Texture2D* debug_motion_source() const noexcept;
    [[nodiscard]] ID3D11Texture2D* debug_depth_source() const noexcept;
    [[nodiscard]] ID3D11Texture2D* debug_color_source() const noexcept;

    [[nodiscard]] ID3D11Texture2D* debug_raw_motion_source() const noexcept;

    [[nodiscard]] ID3D11Texture2D*
        debug_generator_motion_source() const noexcept;
    [[nodiscard]] bool evaluation_failed() noexcept;
    void begin_frame() noexcept;
    void reset_history() noexcept;
    [[nodiscard]] bool presentation_prepared() const noexcept;
    [[nodiscard]] bool scene_ever_finalized() const noexcept;
    [[nodiscard]] bool evaluate_present_surface(
        ID3D11Texture2D* color,
        ID3D11Texture2D* output);

    [[nodiscard]] bool request_same_extent_provider_switch(
        providers::Vendor provider,
        config::UpscalingMode mode,
        bool& restart_required,
        std::string& detail);

    [[nodiscard]] bool scene_resolve_pending() const noexcept;

    bool commit_pending_scene_resolve(const char* boundary);

private:
    struct State;

    UpscalingPass();
    ~UpscalingPass();
    UpscalingPass(const UpscalingPass&) = delete;
    UpscalingPass& operator=(const UpscalingPass&) = delete;

    static void main_draw_thunk(std::int64_t renderer, int unknown);
    static void post_processing_thunk(
        RE::ImageSpaceManager* manager,
        std::uint32_t unknown,
        RE::RENDER_TARGET target,
        void* data,
        bool flag);

    void capture_hudless_back_buffer();
    [[nodiscard]] bool evaluate_pre_postprocessing_scene();
    [[nodiscard]] bool evaluate_provider_surface(
        ID3D11Texture2D* color,
        ID3D11Texture2D* output,
        providers::Vendor provider,
        config::UpscalingMode mode,
        std::uint32_t render_width,
        std::uint32_t render_height,
        std::uint32_t output_width,
        std::uint32_t output_height);

    void note_switch_recovery() noexcept;

    static constexpr std::uint32_t kSwitchRecoveryFrameBudget = 120U;

    static constexpr std::uint32_t kDepthContractGraceFrames = 60U;

    using MainDrawFunction = void(std::int64_t, int);
    using PostProcessingFunction = void(
        RE::ImageSpaceManager*,
        std::uint32_t,
        RE::RENDER_TARGET,
        void*,
        bool);
    static inline REL::Relocation<MainDrawFunction> original_main_draw_;
    static inline REL::Relocation<PostProcessingFunction>
        original_post_processing_;

    bool installed_{};
    bool reset_next_evaluation_{true};
    bool evaluation_verified_{};
    bool evaluation_failed_{};
    bool first_evaluation_logged_{};
    bool first_callsite_logged_{};
    bool presentation_prepared_{};
    bool presentation_fallback_logged_{};
    bool attempted_this_frame_{};

    bool scene_finalized_this_frame_{};
    bool scene_ever_finalized_{};

    bool scene_resolve_pending_{};
    bool extra_framebuffer_pass_logged_{};
    bool native_ui_phase_logged_{};
    bool late_boundary_logged_{};
    providers::Vendor active_upscaling_provider_{providers::Vendor::none};
    bool provider_change_logged_{};
    bool provider_failure_logged_{};

    bool foreign_provider_route_logged_{};

    bool depth_refusal_logged_{};

    std::uint32_t depth_indeterminate_frames_{};

    bool provider_contract_logged_{};

    bool switch_recovery_armed_{};
    providers::Vendor switch_recovery_provider_{providers::Vendor::none};
    config::UpscalingMode switch_recovery_mode_{config::UpscalingMode::off};
    std::uint32_t switch_recovery_render_width_{};
    std::uint32_t switch_recovery_render_height_{};
    std::uint32_t switch_recovery_frames_remaining_{};
    std::uint32_t diagnostic_mode_{};
    std::uint32_t diagnostic_viewport_width_{};
    std::uint32_t diagnostic_viewport_height_{};
    std::uint32_t retry_frames_remaining_{};
    std::unique_ptr<State> state_;
    std::unique_ptr<State> present_state_;
};
}
