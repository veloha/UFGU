#pragma once

#include "render/ContractSnapshot.hpp"

#include <cstdint>

namespace sl
{
struct FrameToken;
}

namespace mfgdlss::streamline
{
class FrameSubmission final
{
public:
    [[nodiscard]] static FrameSubmission& instance() noexcept;

    [[nodiscard]] bool initialize_timing(
        std::uint32_t output_width,
        std::uint32_t output_height);
    [[nodiscard]] bool begin_frame();
    [[nodiscard]] bool ensure_constants(
        bool reset = false,
        bool motion_vectors_dilated = false);
    [[nodiscard]] bool submit();

    [[nodiscard]] bool submit_inactive_tags();
    void request_history_reset() noexcept;
    void set_reflex_sleep_enabled(bool enabled) noexcept;
    void render_submit_start();
    void render_submit_end();
    void present_start();
    void present_end();
    void abandon_frame() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] const sl::FrameToken* frame_token() const noexcept;
    [[nodiscard]] std::uint32_t frame_index() const noexcept;

private:
    [[nodiscard]] bool acquire_frame_token();
    void set_marker(std::uint32_t marker);
    void close_simulation_marker();

    sl::FrameToken* frame_token_{};
    void* reflex_sleep_{};
    void* pcl_set_marker_{};
    void* pcl_get_state_{};
    void* pcl_set_options_{};
    std::uint32_t frame_index_{};
    std::uint32_t presentation_index_{};
    std::uint32_t output_width_{};
    std::uint32_t output_height_{};
    std::uint64_t begin_call_count_{};
    std::uint64_t opened_frame_count_{};
    std::uint64_t present_count_{};
    std::uint64_t duplicate_begin_count_{};
    std::uint64_t reported_opened_frames_{};
    std::uint64_t reported_presents_{};
    std::uint64_t reported_duplicate_begins_{};
    std::uint64_t last_camera_sequence_{};
    bool frame_open_{};
    bool frame_prepared_{};
    bool simulation_open_{};
    bool constants_submitted_{};
    bool reset_pending_{true};
    bool timing_ready_{};
    std::uint32_t pcl_ping_message_{};
    bool reflex_sleep_enabled_{true};
    bool duplicate_begin_logged_{};
    bool stale_camera_logged_{};
    bool lifecycle_logged_{};
    bool first_submission_logged_{};
    bool inactive_tags_logged_{};
    bool inactive_tag_failure_logged_{};
    bool failure_logged_{};
    bool reduced_viewport_rejection_logged_{};
    render::ContractSnapshot tag_contract_{"Frame generation tag"};
    bool zero_extent_logged_{};
    bool reduced_ui_rejection_logged_{};
};
}
