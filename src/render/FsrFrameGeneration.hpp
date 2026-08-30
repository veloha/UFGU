#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace mfgdlss::render
{
enum class FsrGenerationState : std::uint32_t
{
    inactive,
    waiting,
    installed,
    failed
};

class FsrFrameGeneration final
{
public:
    [[nodiscard]] static FsrFrameGeneration& instance() noexcept;

    [[nodiscard]] static bool selected() noexcept;

    [[nodiscard]] bool install_if_ready(std::uint32_t generated_frames);

    [[nodiscard]] bool submit_frame(
        void* command_list,
        std::uint32_t render_width,
        std::uint32_t render_height,
        std::uint32_t output_width,
        std::uint32_t output_height,
        float frame_time_milliseconds,
        bool reset_history);

    void set_enabled(bool enabled) noexcept;
    void reset_history() noexcept;
    [[nodiscard]] std::uint64_t estimated_vram_bytes() const noexcept;

    void shutdown() noexcept;

    [[nodiscard]] FsrGenerationState state() const noexcept;
    [[nodiscard]] bool owns_presentation() const noexcept;
    [[nodiscard]] const std::string& version() const noexcept;
    [[nodiscard]] const std::string& detail() const noexcept;
    [[nodiscard]] std::uint32_t generated_frames() const noexcept;

    [[nodiscard]] static std::uint32_t maximum_multiplier() noexcept;

private:
    FsrFrameGeneration();
    ~FsrFrameGeneration();

    struct State;
    std::unique_ptr<State> state_;
};
}
