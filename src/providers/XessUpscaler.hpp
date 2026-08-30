#pragma once

#include "providers/ProviderTypes.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace mfgdlss::providers
{
struct XessDispatch
{
    void* color{};
    void* motion_vectors{};
    void* depth{};
    void* responsive_mask{};
    void* output{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    float jitter_x{};
    float jitter_y{};
    bool reset_history{};
    bool reversed_depth{};
};

class XessUpscaler final
{
public:
    ~XessUpscaler();

    [[nodiscard]] static XessUpscaler& instance() noexcept;

    [[nodiscard]] bool query_render_resolution(
        QualityMode quality,
        std::uint32_t output_width,
        std::uint32_t output_height,
        std::uint32_t& render_width,
        std::uint32_t& render_height);

    [[nodiscard]] bool supports_render_resolution(
        QualityMode quality,
        std::uint32_t output_width,
        std::uint32_t output_height,
        std::uint32_t render_width,
        std::uint32_t render_height);
    [[nodiscard]] bool configure(
        QualityMode quality,
        std::uint32_t render_width,
        std::uint32_t render_height,
        std::uint32_t output_width,
        std::uint32_t output_height,
        bool reversed_depth);
    [[nodiscard]] bool evaluate(const XessDispatch& dispatch);
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] Availability availability() const noexcept;
    [[nodiscard]] const std::string& version() const noexcept;
    [[nodiscard]] const std::string& detail() const noexcept;
    [[nodiscard]] std::uint32_t expected_render_width() const noexcept;
    [[nodiscard]] std::uint32_t expected_render_height() const noexcept;

private:
    struct State;

    [[nodiscard]] bool query_input_resolution_range(
        QualityMode quality,
        std::uint32_t output_width,
        std::uint32_t output_height,
        std::uint32_t& optimal_width,
        std::uint32_t& optimal_height,
        std::uint32_t& minimum_width,
        std::uint32_t& minimum_height,
        std::uint32_t& maximum_width,
        std::uint32_t& maximum_height);
    std::unique_ptr<State> state_;
    Availability availability_{Availability::unknown};
    std::string version_;
    std::string detail_;
};
}
