#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace mfgdlss::render
{

enum class XessGenerationState : std::uint32_t
{

    inactive,

    waiting,
    installed,
    failed
};

class XessFrameGeneration final
{
public:
    [[nodiscard]] static XessFrameGeneration& instance() noexcept;

    [[nodiscard]] static bool selected() noexcept;

    [[nodiscard]] bool install_if_ready(std::uint32_t interpolated_frames);

    [[nodiscard]] bool submit_frame(
        std::uint32_t render_width,
        std::uint32_t render_height,
        std::uint32_t output_width,
        std::uint32_t output_height,
        float frame_time_milliseconds,
        bool reset_history);

    void set_enabled(bool enabled) noexcept;
    void reset_history() noexcept;
    enum class LatencyMarker : std::uint32_t
    {
        simulation_start = 0U,
        simulation_end = 1U,
        render_submit_start = 2U,
        render_submit_end = 3U,
        present_start = 4U,
        present_end = 5U
    };

    void begin_latency_frame() noexcept;
    void add_latency_marker(LatencyMarker marker) noexcept;
    void set_output_target_fps(std::uint32_t frames_per_second) noexcept;

    void shutdown() noexcept;

    [[nodiscard]] XessGenerationState state() const noexcept;
    [[nodiscard]] bool owns_presentation() const noexcept;
    [[nodiscard]] const std::string& version() const noexcept;
    [[nodiscard]] const std::string& detail() const noexcept;
    [[nodiscard]] std::uint64_t estimated_vram_bytes() const noexcept;
    [[nodiscard]] std::uint32_t maximum_interpolated_frames() const noexcept;

    [[nodiscard]] std::uint32_t maximum_multiplier() const noexcept;

    [[nodiscard]] bool probe_capability();
    [[nodiscard]] std::uint32_t presented_frame_count() const noexcept;

private:
    XessFrameGeneration();
    ~XessFrameGeneration();

    struct State;
    std::unique_ptr<State> state_;
};
}
