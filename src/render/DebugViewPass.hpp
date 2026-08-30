#pragma once

#include "render/DebugViewCore.hpp"
#include "render/ResourceProbe.hpp"

#include <cstdint>

struct ID3D11Texture2D;

namespace mfgdlss::render
{
class DebugViewPass final
{
public:
    DebugViewPass();
    ~DebugViewPass();

    DebugViewPass(const DebugViewPass&) = delete;
    DebugViewPass& operator=(const DebugViewPass&) = delete;

    [[nodiscard]] static DebugViewPass& instance() noexcept;

    [[nodiscard]] static bool armed() noexcept;

    [[nodiscard]] static DebugView selected_view() noexcept;
    static void select(DebugView view) noexcept;

    [[nodiscard]] bool render(ID3D11Texture2D* target);

    void shutdown() noexcept;

    [[nodiscard]] DebugViewStatus last_status() const noexcept;

    [[nodiscard]] const char* unavailable_reason() const noexcept;

    struct SourceInfo
    {
        std::uint32_t source_width{};
        std::uint32_t source_height{};
        std::uint32_t source_format{};
        std::uint32_t sampled_format{};
        std::uint32_t active_width{};
        std::uint32_t active_height{};
        std::uint32_t output_width{};
        std::uint32_t output_height{};
        float motion_scale_x{};
        float motion_scale_y{};
        bool reversed_depth{};
        bool valid{};
    };
    [[nodiscard]] SourceInfo source_info() const noexcept;

    [[nodiscard]] const ProbeStatistics& motion_statistics() const noexcept;

    [[nodiscard]] std::uint64_t frames_since_select() const noexcept;

private:

    static constexpr std::uint64_t kMotionCaptureIntervalFrames = 20;
    static constexpr float kMotionHistogramScale = 0.25F;

    static constexpr std::uint32_t kMotionStatisticsLogBudget = 24;

    DebugViewRenderer renderer_;
    ResourceProbe motion_probe_;

    ResourceProbe raw_motion_probe_;
    DebugViewStatus last_status_{DebugViewStatus::off};
    SourceInfo source_info_{};
    DebugViewStatus logged_status_{DebugViewStatus::off};
    DebugView logged_view_{DebugView::off};
    std::uint64_t frames_since_select_{};
    std::uint64_t next_motion_capture_frame_{};
    std::uint32_t motion_statistics_logged_{};
    float last_logged_motion_maximum_{-1.0F};
    float last_logged_raw_motion_maximum_{-1.0F};
};
}
