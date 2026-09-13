#pragma once

#include "render/JitterOwnership.hpp"

#include <d3d11.h>
#include <RE/Skyrim.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace mfgdlss::render
{

struct FoldGateObservation final
{
    float projection_scale_x{};
    float projection_scale_y{};

    bool state_available{};

    bool taa_flag_available{};
    bool taa_flag_enabled{};
};

class DynamicResolution final :
    public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
    [[nodiscard]] static DynamicResolution& instance() noexcept;

    [[nodiscard]] bool install();
    [[nodiscard]] bool attach_context(ID3D11DeviceContext* context);
    void shutdown() noexcept;

    [[nodiscard]] float jitter_x() const noexcept;
    [[nodiscard]] float jitter_y() const noexcept;

    [[nodiscard]] FoldGateObservation observe_fold_gate() const noexcept;

    [[nodiscard]] bool jitter_fold_patched() const noexcept
    {
        return jitter_fold_patched_;
    }

    [[nodiscard]] float engine_projection_scale_x() const noexcept;
    [[nodiscard]] float engine_projection_scale_y() const noexcept;

    struct ProjectionScaleTrace final
    {
        float resolution_entry_x{};
        float resolution_entry_y{};
        float resolution_exit_x{};
        float resolution_exit_y{};
        float jitter_entry_x{};
        float jitter_entry_y{};
        float jitter_exit_x{};
        float jitter_exit_y{};
        bool resolution_seen{};
    };
    [[nodiscard]] bool should_evaluate() const noexcept;
    [[nodiscard]] bool scene_viewport_verified() const noexcept;
    [[nodiscard]] bool ui_target_redirection_failed() const noexcept;

    [[nodiscard]] bool full_resolution_ui_active() const noexcept;

    void arm_frame_episode_trace() noexcept;
    void finalize_frame_episode_trace(ID3D11DeviceContext* context) noexcept;

    void note_production_ui_phase_end_boundary() noexcept;

    void arm_extent_observation(bool armed) noexcept;
    [[nodiscard]] bool extent_observation_armed() const noexcept;
    [[nodiscard]] bool extent_violation_observed() const noexcept;
    [[nodiscard]] const char* extent_violation_reason() const noexcept;
    [[nodiscard]] bool extent_violation_rectangle(
        std::int32_t& left,
        std::int32_t& top,
        std::int32_t& right,
        std::int32_t& bottom) const noexcept;

    void prepare_ui() noexcept;

    void enable_vanilla_taa_for_world_render() noexcept;

    void suppress_vanilla_taa_resolve() noexcept;
    void begin_full_resolution_ui() noexcept;
    void end_full_resolution_ui() noexcept;
    void begin_full_resolution_post_processing() noexcept;
    void end_full_resolution_post_processing() noexcept;

    RE::BSEventNotifyControl ProcessEvent(
        const RE::MenuOpenCloseEvent* event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>* source) override;

private:
    static void update_resolution_thunk(RE::BSGraphics::State* state);
    static void update_jitter_thunk(RE::BSGraphics::State* state);
    static void STDMETHODCALLTYPE rs_set_viewports_thunk(
        ID3D11DeviceContext* context,
        UINT viewport_count,
        const D3D11_VIEWPORT* viewports);
    static void STDMETHODCALLTYPE rs_set_scissor_rects_thunk(
        ID3D11DeviceContext* context,
        UINT rectangle_count,
        const D3D11_RECT* rectangles);
    static void STDMETHODCALLTYPE om_set_render_targets_thunk(
        ID3D11DeviceContext* context,
        UINT target_count,
        ID3D11RenderTargetView* const* targets,
        ID3D11DepthStencilView* depth);
    static void STDMETHODCALLTYPE
    om_set_render_targets_and_unordered_access_views_thunk(
        ID3D11DeviceContext* context,
        UINT target_count,
        ID3D11RenderTargetView* const* targets,
        ID3D11DepthStencilView* depth,
        UINT unordered_start_slot,
        UINT unordered_count,
        ID3D11UnorderedAccessView* const* unordered_views,
        const UINT* initial_counts);
    static void STDMETHODCALLTYPE clear_depth_stencil_view_thunk(
        ID3D11DeviceContext* context,
        ID3D11DepthStencilView* depth,
        UINT clear_flags,
        FLOAT depth_value,
        UINT8 stencil_value);

    void open_ui_phase_episode(
        ID3D11DeviceContext* context,
        bool implicit_checkpoint = false) noexcept;
    void close_ui_phase_episode(ID3D11DeviceContext* context) noexcept;

    void mark_frame_episode_phase(bool begin) noexcept;

    void note_ui_bind(
        ID3D11DeviceContext* context,
        const void* caller_address,
        bool unordered_variant) noexcept;

    void report_declined_ui_target(
        unsigned target_count,
        unsigned failing_index,
        ID3D11RenderTargetView* const* targets,
        ID3D11RenderTargetView* const* adjusted_targets,
        ID3D11DepthStencilView* depth) noexcept;

    void report_scene_domain_bind(
        unsigned target_count,
        unsigned scene_index,
        ID3D11RenderTargetView* const* targets) noexcept;
    void report_external_overlay_bind(
        HMODULE overlay,
        UINT target_count,
        ID3D11RenderTargetView* const* targets) noexcept;
    [[nodiscard]] bool remap_full_resolution_ui_targets(
        UINT target_count,
        ID3D11RenderTargetView* const* targets,
        ID3D11DepthStencilView* depth,
        ID3D11RenderTargetView** adjusted_targets,
        ID3D11DepthStencilView*& adjusted_depth) noexcept;
    void update_resolution(RE::BSGraphics::State* state) noexcept;
    void reapply_scene_scale(RE::BSGraphics::State* state) noexcept;
    void update_dynamic_resolution_mode(
        bool dlss_enabled,
        bool reduced_resolution) noexcept;
    void update_jitter(RE::BSGraphics::State* state) noexcept;
    void observe_viewports(
        ID3D11DeviceContext* context,
        UINT viewport_count,
        const D3D11_VIEWPORT* viewports) noexcept;
    void observe_current_viewports(
        ID3D11DeviceContext* context) noexcept;
    void enforce_virtual_render_extent(
        ID3D11DeviceContext* context) noexcept;
    [[nodiscard]] bool full_resolution_target_bound(
        ID3D11DeviceContext* context,
        std::uint32_t* width = nullptr,
        std::uint32_t* height = nullptr) const noexcept;
    void enforce_full_resolution_extent(
        ID3D11DeviceContext* context) noexcept;
    void log_post_processing_promotion(
        bool viewport_promoted,
        bool scissor_promoted) noexcept;
    void update_vanilla_taa(bool dlss_enabled) noexcept;
    void restore_full_resolution(
        RE::BSGraphics::State* state,
        bool restore_projection) noexcept;

    using StateFunction = void(RE::BSGraphics::State*);
    using RsSetViewportsFunction = void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*,
        UINT,
        const D3D11_VIEWPORT*);
    using RsSetScissorRectsFunction = void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*,
        UINT,
        const D3D11_RECT*);
    using OmSetRenderTargetsFunction = void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*,
        UINT,
        ID3D11RenderTargetView* const*,
        ID3D11DepthStencilView*);
    using OmSetRenderTargetsAndUnorderedAccessViewsFunction =
        void(STDMETHODCALLTYPE*)(
            ID3D11DeviceContext*,
            UINT,
            ID3D11RenderTargetView* const*,
            ID3D11DepthStencilView*,
            UINT,
            UINT,
            ID3D11UnorderedAccessView* const*,
            const UINT*);
    using ClearDepthStencilViewFunction = void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*,
        ID3D11DepthStencilView*,
        UINT,
        FLOAT,
        UINT8);
    static inline REL::Relocation<StateFunction> original_update_resolution_;
    static inline REL::Relocation<StateFunction> original_update_jitter_;
    static inline RsSetViewportsFunction original_rs_set_viewports_{};
    static inline RsSetScissorRectsFunction original_rs_set_scissor_rects_{};
    static inline OmSetRenderTargetsFunction original_om_set_render_targets_{};
    static inline OmSetRenderTargetsAndUnorderedAccessViewsFunction
        original_om_set_render_targets_and_unordered_access_views_{};
    static inline ClearDepthStencilViewFunction
        original_clear_depth_stencil_view_{};

    RE::Setting* dynamic_resolution_setting_{};
    RE::Setting* dynamic_resolution_clamp_setting_{};
    RE::Setting* temporal_aa_setting_{};
    std::uint32_t jitter_index_{};
    std::uint32_t active_frame_{UINT32_MAX};
    std::uint32_t expected_viewport_width_{};
    std::uint32_t expected_viewport_height_{};

    std::uint32_t viewport_diagnostic_budget_{24};
    std::uint32_t observed_viewport_samples_{};
    std::uint32_t observed_main_bound_samples_{};
    std::uint32_t closest_viewport_width_{};
    std::uint32_t closest_viewport_height_{};
    float closest_viewport_left_{};
    float closest_viewport_top_{};
    long long closest_viewport_distance_{-1};
    float jitter_x_{};
    float jitter_y_{};
    float original_projection_scale_x_{};
    float original_projection_scale_y_{};
    ProjectionScaleTrace projection_scale_trace_{};
    BoundedLogBudget projection_scale_trace_budget_{8U};
    float scene_scale_{1.0F};
    float last_scene_scale_{1.0F};
    float original_dynamic_resolution_clamp_{};
    std::int32_t original_dynamic_resolution_lock_{};
    bool original_dynamic_resolution_enabled_{};
    bool original_temporal_aa_enabled_{};
    bool original_runtime_temporal_aa_enabled_{};
    bool menu_override_{};
    bool should_evaluate_{};
    bool expected_viewport_seen_{};
    bool viewport_hook_installed_{};
    bool dynamic_resolution_mode_initialized_{};
    bool dynamic_resolution_active_{};
    bool temporal_aa_override_active_{};
    bool temporal_aa_override_logged_{};

    JitterOwnershipState jitter_ownership_{};
    TemporalAaPolicy temporal_aa_policy_logged_{
        TemporalAaPolicy::restore_user_setting};
    bool runtime_temporal_state_captured_{};
    bool runtime_temporal_state_logged_{};

    bool runtime_temporal_state_last_logged_{};
    bool runtime_taa_flag_absent_logged_{};
    bool runtime_taa_write_rejected_logged_{};
    bool full_resolution_restore_logged_{};
    bool full_resolution_ui_{};
    bool ui_resolution_promotion_logged_{};
    bool ui_target_redirection_logged_{};

    bool ui_target_redirection_failed_{};

    bool framebuffer_outside_display_logged_{};
    bool ui_target_failure_logged_{};

    bool ui_target_declined_this_frame_{};
    std::uint32_t declined_ui_target_reports_{};
    std::uint32_t scene_domain_bind_reports_{};
    std::array<void*, 4> external_overlay_modules_logged_{};
    bool orphan_episode_logged_{};

    ID3D11DeviceContext* declined_probe_context_{};
    const void* declined_caller_address_{};
    bool declined_caller_unordered_{};
    std::uint32_t ui_bind_index_{};
    bool full_resolution_post_processing_{};
    bool post_processing_guard_armed_logged_{};
    bool post_processing_promotion_logged_{};
    bool post_processing_guard_summary_logged_{};
    bool virtual_viewport_correction_logged_{};
    bool virtual_scissor_correction_logged_{};

    std::atomic<bool> extent_observation_armed_{false};
    std::atomic<bool> extent_violation_observed_{false};
    const char* extent_violation_reason_{};
    std::int32_t extent_violation_left_{};
    std::int32_t extent_violation_top_{};
    std::int32_t extent_violation_right_{};
    std::int32_t extent_violation_bottom_{};
    void note_extent_violation(
        const char* reason,
        std::int32_t left,
        std::int32_t top,
        std::int32_t right,
        std::int32_t bottom) noexcept;
    bool installed_{};
    bool jitter_fold_patched_{};
};
}
