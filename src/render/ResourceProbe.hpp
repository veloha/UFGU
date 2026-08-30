#pragma once

#include <cstdint>
#include <memory>

#include <d3d11.h>

namespace mfgdlss::render
{

inline constexpr std::uint32_t kMaximumSamples = 262144;

inline constexpr std::uint32_t kCollectionFrameBudget = 16;

inline constexpr std::uint32_t kProbeHistogramBuckets = 16;

enum class ProbeChannel : std::uint32_t
{

    depth = 0,

    motion = 1,
};

enum class ProbeStatus : std::uint32_t
{

    complete = 0,

    idle,

    pending,
    abandoned_readback_timed_out,
    no_device,
    no_source,
    source_format_unsupported,
    source_not_shader_readable,
    shader_compilation_failed,
    resource_creation_failed,
};

[[nodiscard]] const char* describe(ProbeStatus status) noexcept;

struct ProbeStatistics
{
    bool valid{};
    ProbeChannel channel{ProbeChannel::depth};
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::uint32_t stride{};
    std::uint32_t sampled_pixels{};

    float minimum{};
    float maximum{};

    std::uint32_t nonfinite_pixels{};

    std::uint32_t out_of_range_pixels{};

    std::uint32_t far_pixels{};

    std::uint32_t near_zero_pixels{};

    std::int32_t bounds_left{};
    std::int32_t bounds_top{};
    std::int32_t bounds_right{};
    std::int32_t bounds_bottom{};

    std::uint32_t quadrant_right_down{};
    std::uint32_t quadrant_right_up{};
    std::uint32_t quadrant_left_down{};
    std::uint32_t quadrant_left_up{};

    std::uint32_t histogram[kProbeHistogramBuckets]{};

    [[nodiscard]] bool bounds_empty() const noexcept
    {
        return bounds_left > bounds_right;
    }

    [[nodiscard]] float bucket_percentile(float fraction) const noexcept;
};

struct ProbeParams
{
    ProbeChannel channel{ProbeChannel::depth};

    bool reversed_depth{true};

    float far_epsilon{};

    float near_zero_threshold{};
    float maximum_expected_magnitude{1.0F};
};

class ResourceProbe final
{
public:
    ResourceProbe();
    ~ResourceProbe();

    ResourceProbe(const ResourceProbe&) = delete;
    ResourceProbe& operator=(const ResourceProbe&) = delete;

    void arm() noexcept;
    [[nodiscard]] bool armed() const noexcept;

    ProbeStatus capture(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* source,
        const ProbeParams& params);

    ProbeStatus collect(ID3D11DeviceContext* context);

    [[nodiscard]] ProbeStatus status() const noexcept;
    [[nodiscard]] const ProbeStatistics& statistics() const noexcept;

    void shutdown() noexcept;
    [[nodiscard]] bool holds_resources() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

[[nodiscard]] std::uint32_t probe_stride_for(
    std::uint32_t width,
    std::uint32_t height) noexcept;
}
