#pragma once

#include <cstdint>

namespace mfgdlss::render
{
[[nodiscard]] bool run_motion_vector_reduction(
    void* d3d12_device,
    void* d3d12_command_queue,
    void* d3d12_motion_texture,
    void* d3d12_depth_texture,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t motion_format,
    std::uint32_t (&results)[16]);

void sample_motion_vector_census();
}
