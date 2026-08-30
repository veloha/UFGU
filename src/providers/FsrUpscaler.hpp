#pragma once

#include "providers/ProviderTypes.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace mfgdlss::providers
{
struct FsrDispatch
{
    void* color{};
    void* motion_vectors{};
    void* depth{};
    void* reactive_mask{};
    void* transparency_mask{};
    void* output{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    float jitter_x{};
    float jitter_y{};
    float camera_near{};
    float camera_far{};
    float camera_fov_vertical{};
    bool reset_history{};
    bool reversed_depth{};
    bool infinite_depth{};
};

class FsrUpscaler final
{
public:
    ~FsrUpscaler();

    [[nodiscard]] static FsrUpscaler& instance() noexcept;

    [[nodiscard]] bool supports_render_resolution(
        QualityMode quality,
        std::uint32_t output_width,
        std::uint32_t output_height,
        std::uint32_t render_width,
        std::uint32_t render_height);
    [[nodiscard]] bool query_render_resolution(
        QualityMode quality,
        std::uint32_t output_width,
        std::uint32_t output_height,
        std::uint32_t& render_width,
        std::uint32_t& render_height);
    [[nodiscard]] bool configure(
        QualityMode quality,
        std::uint32_t render_width,
        std::uint32_t render_height,
        std::uint32_t output_width,
        std::uint32_t output_height,
        bool reversed_depth,
        bool infinite_depth);
    [[nodiscard]] bool evaluate(const FsrDispatch& dispatch);
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] Availability availability() const noexcept;
    [[nodiscard]] const std::string& version() const noexcept;
    [[nodiscard]] const std::string& detail() const noexcept;
    [[nodiscard]] std::uint32_t expected_render_width() const noexcept;
    [[nodiscard]] std::uint32_t expected_render_height() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
    Availability availability_{Availability::unknown};
    std::string version_;
    std::string detail_;
};
}
