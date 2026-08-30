#pragma once

#include <cstdint>

namespace mfgdlss::streamline
{
class FeatureSupport final
{
public:
    [[nodiscard]] static FeatureSupport& instance() noexcept;

    [[nodiscard]] bool initialize(
        std::uint32_t output_width,
        std::uint32_t output_height);
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::uint32_t maximum_multiplier() const noexcept;
    [[nodiscard]] bool dynamic_mfg_supported() const noexcept;
    [[nodiscard]] std::uint64_t estimated_vram_bytes() const noexcept;
    void refresh_vram_estimate(
        std::uint32_t output_width,
        std::uint32_t output_height) noexcept;

private:
    bool ready_{};
    std::uint32_t maximum_multiplier_{1};
    bool dynamic_mfg_supported_{};
    std::uint64_t estimated_vram_bytes_{};
};
}
