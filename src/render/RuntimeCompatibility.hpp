#pragma once

#include "render/HookPreflight.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mfgdlss::render
{
struct RuntimeVersion final
{
    std::uint16_t major{};
    std::uint16_t minor{};
    std::uint16_t patch{};
    std::uint16_t build{};

    [[nodiscard]] friend constexpr bool operator==(
        const RuntimeVersion&,
        const RuntimeVersion&) noexcept = default;
};

struct StateLayout final
{
    std::ptrdiff_t projection_scale_x{};
    std::ptrdiff_t projection_scale_y{};
    std::ptrdiff_t frame_count{};
    std::ptrdiff_t dynamic_width_scale{};
    std::ptrdiff_t dynamic_height_scale{};
    std::ptrdiff_t previous_width_scale{};
    std::ptrdiff_t previous_height_scale{};
    std::ptrdiff_t dynamic_resolution_lock{};
    std::ptrdiff_t image_space_runtime_state{};
    std::ptrdiff_t runtime_taa_enabled{};
};

struct CameraLayout final
{
    std::ptrdiff_t near_plane{};
    std::ptrdiff_t far_plane{};
    std::size_t frame_buffer_bytes{};
};

struct AddressLibraryIds final
{
    std::uint64_t graphics_state{};
    std::uint64_t image_space_state{};
    std::uint64_t update_camera_data{};
    std::uint64_t per_frame_buffer{};
    std::uint64_t camera_parameters{};
    std::uint64_t set_dirty_states{};
    std::uint64_t dynamic_resolution_update{};
    std::uint64_t temporal_jitter_update{};
    std::uint64_t jitter_update_gate{};
    std::uint64_t camera_state_build{};
    std::uint64_t main_world_draw{};
    std::uint64_t post_processing{};
};

struct HookLayout final
{
    std::ptrdiff_t resolution_call{};
    CodeSignature resolution_call_signature{};
    std::ptrdiff_t jitter_call{};
    CodeSignature jitter_call_signature{};
    std::ptrdiff_t main_draw_call{};
    CodeSignature main_draw_call_signature{};
    std::ptrdiff_t post_processing_call{};
    CodeSignature post_processing_call_signature{};
    std::ptrdiff_t jitter_update_patch{};
    CodeSignature jitter_update_patch_signature{};
    std::ptrdiff_t camera_state_patch{};
    CodeSignature camera_state_patch_signature{};
};

struct ScaleformLayout final
{
    std::size_t begin_display_slot{};
    std::size_t end_display_slot{};
    bool abi_validated{};
};

struct RuntimeProfile final
{
    RuntimeVersion version{};
    std::string_view name{};
    bool jitter_fold_patch_supported{};
    StateLayout state{};
    CameraLayout camera{};
    AddressLibraryIds address_ids{};
    HookLayout hooks{};
    ScaleformLayout scaleform{};
};

inline constexpr RuntimeProfile kSkyrim1597{
    .version = {1, 5, 97, 0},
    .name = "Skyrim SE 1.5.97",
    .jitter_fold_patch_supported = true,
    .state = {
        .projection_scale_x = 0x44,
        .projection_scale_y = 0x48,
        .frame_count = 0x4C,
        .dynamic_width_scale = 0xFC,
        .dynamic_height_scale = 0x100,
        .previous_width_scale = 0x104,
        .previous_height_scale = 0x108,
        .dynamic_resolution_lock = 0x110,
        .image_space_runtime_state = 0x1F0,
        .runtime_taa_enabled = 0x18,
    },
    .camera = {
        .near_plane = 0x40,
        .far_plane = 0x44,
        .frame_buffer_bytes = 720,
    },
    .address_ids = {
        .graphics_state = 524998,
        .image_space_state = 527731,
        .update_camera_data = 75472,
        .per_frame_buffer = 524768,
        .camera_parameters = 517032,
        .set_dirty_states = 75580,
        .dynamic_resolution_update = 35556,
        .temporal_jitter_update = 75460,
        .jitter_update_gate = 75709,
        .camera_state_build = 75711,
        .main_world_draw = 79947,
        .post_processing = 100430,
    },
    .hooks = {
        .resolution_call = 0x2D,
        .resolution_call_signature = {
            .relative_offset = -8,
            .length = 24,
            .bytes = {
                0xE9, 0x48, 0x8D, 0x0D, 0x43, 0xB8, 0xA7, 0x02,
                0xE8, 0xEE, 0xBD, 0x7C, 0x00, 0x48, 0x8D, 0x0D,
                0x37, 0x74, 0xA7, 0x02, 0xE8, 0x22, 0x90, 0x7B,
            },
            .mask = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            },
            .expected_target_rva = 0xD7CE40,
            .approved = true,
        },
        .jitter_call = 0xE5,
        .jitter_call_signature = {
            .relative_offset = -8,
            .length = 24,
            .bytes = {
                0x02, 0xC6, 0x05, 0x3B, 0x27, 0x2C, 0x02, 0x01,
                0xE8, 0x06, 0x2E, 0x01, 0x00, 0x48, 0x8B, 0x0D,
                0xEF, 0xDC, 0x2B, 0x02, 0x48, 0x8B, 0x01, 0xFF,
            },
            .mask = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            },
            .expected_target_rva = 0xD7CFB0,
            .approved = true,
        },
        .main_draw_call = 0x16F,
        .main_draw_call_signature = {
            .relative_offset = -8,
            .length = 24,
            .bytes = {
                0xD2, 0x48, 0x8D, 0x0D, 0x11, 0x8E, 0x16, 0x02,
                0xE8, 0xAC, 0xAC, 0xEA, 0xFF, 0x48, 0x8B, 0x05,
                0x15, 0x61, 0x06, 0x02, 0x48, 0x8B, 0x48, 0x10,
            },
            .mask = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            },
            .expected_target_rva = 0xD6A330,
            .approved = true,
        },
        .post_processing_call = 0x1F0,
        .post_processing_call_signature = {
            .relative_offset = -8,
            .length = 24,
            .bytes = {
                0x00, 0x48, 0x8B, 0x0D, 0x10, 0xC6, 0xEE, 0x01,
                0xE8, 0x7B, 0x28, 0xFB, 0xFF, 0x48, 0x8B, 0x05,
                0x1C, 0xE9, 0xF4, 0x01, 0x48, 0x8B, 0x88, 0x28,
            },
            .mask = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            },
            .expected_target_rva = 0x1297410,
            .approved = true,
        },
        .jitter_update_patch = 0x0E,
        .jitter_update_patch_signature = {
            .length = 6,
            .bytes = {0x80, 0x7A, 0x18, 0x00, 0x74, 0x7A},
            .mask = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
            .approved = true,
        },
        .camera_state_patch = 0x1D5,
        .camera_state_patch_signature = {
            .length = 10,
            .bytes = {
                0x80, 0x79, 0x18, 0x00, 0x0F,
                0x84, 0xC0, 0x00, 0x00, 0x00,
            },
            .mask = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            },
            .approved = true,
        },
    },
    .scaleform = {
        .begin_display_slot = 12,
        .end_display_slot = 13,
        .abi_validated = true,
    },
};

inline constexpr RuntimeProfile kSkyrim161170{
    .version = {1, 6, 1170, 0},
    .name = "Skyrim AE 1.6.1170",
    .jitter_fold_patch_supported = true,
    .state = {
        .projection_scale_x = 0x44,
        .projection_scale_y = 0x48,
        .frame_count = 0x4C,
        .dynamic_width_scale = 0x104,
        .dynamic_height_scale = 0x108,
        .previous_width_scale = 0x10C,
        .previous_height_scale = 0x110,
        .dynamic_resolution_lock = 0x118,
        .image_space_runtime_state = 0x1F0,
        .runtime_taa_enabled = 0x18,
    },
    .camera = {
        .near_plane = 0x40,
        .far_plane = 0x44,
        .frame_buffer_bytes = 720,
    },
    .address_ids = {
        .graphics_state = 411479,
        .image_space_state = 414660,
        .update_camera_data = 77258,
        .per_frame_buffer = 411384,
        .camera_parameters = 403540,
        .set_dirty_states = 77386,
        .dynamic_resolution_update = 36555,
        .temporal_jitter_update = 77245,
        .jitter_update_gate = 77518,
        .camera_state_build = 77520,
        .main_world_draw = 82084,
        .post_processing = 107148,
    },
    .hooks = {
        .resolution_call = 0x2D,
        .resolution_call_signature = {
            .relative_offset = -8,
            .length = 24,
            .bytes = {
                0xE9, 0x48, 0x8D, 0x0D, 0xF3, 0x8F, 0xC4, 0x02,
                0xE8, 0xBE, 0x4B, 0x81, 0x00, 0x48, 0x8D, 0x0D,
                0x87, 0x4B, 0xC4, 0x02, 0xE8, 0x12, 0x09, 0x80,
            },
            .mask = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            },
            .expected_target_rva = 0xE587F0,
            .approved = true,
        },
        .jitter_call = 0xE2,
        .jitter_call_signature = {
            .relative_offset = -8,
            .length = 24,
            .bytes = {
                0x02, 0xC6, 0x05, 0x02, 0x86, 0x44, 0x02, 0x01,
                0xE8, 0x99, 0x43, 0x01, 0x00, 0x48, 0x8B, 0x0D,
                0x32, 0x41, 0x44, 0x02, 0x48, 0x8B, 0x01, 0xFF,
            },
            .mask = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            },
            .expected_target_rva = 0xE58A10,
            .approved = true,
        },
        .main_draw_call = 0x17A,
        .main_draw_call_signature = {
            .relative_offset = -8,
            .length = 24,
            .bytes = {
                0xD2, 0x48, 0x8D, 0x0D, 0x46, 0x37, 0x2E, 0x02,
                0xE8, 0xD1, 0xF7, 0xE9, 0xFF, 0x48, 0x8B, 0x05,
                0x42, 0xC1, 0x64, 0x02, 0x48, 0x8B, 0x48, 0x10,
            },
            .mask = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            },
            .expected_target_rva = 0xE44850,
            .approved = true,
        },
        .post_processing_call = 0x1E7,
        .post_processing_call_signature = {
            .relative_offset = -8,
            .length = 24,
            .bytes = {
                0x00, 0x48, 0x8B, 0x0D, 0x29, 0xBD, 0xE5, 0x01,
                0xE8, 0x04, 0x32, 0xFB, 0xFF, 0x48, 0x8B, 0x05,
                0x65, 0xDF, 0xEB, 0x01, 0x48, 0x8B, 0x88, 0x28,
            },
            .mask = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            },
            .expected_target_rva = 0x1481B80,
            .approved = true,
        },
        .jitter_update_patch = 0x11,
        .jitter_update_patch_signature = {
            .length = 6,
            .bytes = {0x80, 0x7A, 0x18, 0x00, 0x74, 0x68},
            .mask = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
            .approved = true,
        },
        .camera_state_patch = 0x1D5,
        .camera_state_patch_signature = {
            .length = 10,
            .bytes = {
                0x80, 0x79, 0x18, 0x00, 0x0F,
                0x84, 0xC0, 0x00, 0x00, 0x00,
            },
            .mask = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            },
            .approved = true,
        },
    },
    .scaleform = {

        .begin_display_slot = 12,
        .end_display_slot = 13,
        .abi_validated = true,
    },
};

[[nodiscard]] constexpr const RuntimeProfile* runtime_profile_for(
    const RuntimeVersion version) noexcept
{
    if (version == kSkyrim1597.version) {
        return &kSkyrim1597;
    }
    if (version == kSkyrim161170.version) {
        return &kSkyrim161170;
    }
    return nullptr;
}
}
