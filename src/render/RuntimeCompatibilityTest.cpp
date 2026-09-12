#include "render/RuntimeCompatibility.hpp"

#include <iterator>

int main() {
  using namespace mfgdlss::render;

  int failures{};
  const auto check = [&failures](const bool condition) {
    failures += condition ? 0 : 1;
  };

  const auto *const se = runtime_profile_for({1, 5, 97, 0});
  const auto *const ae = runtime_profile_for({1, 6, 1170, 0});
  check(se == &kSkyrim1597);
  check(ae == &kSkyrim161170);
  check(se != ae);
  check(se->version == RuntimeVersion{1, 5, 97, 0});
  check(ae->version == RuntimeVersion{1, 6, 1170, 0});
  check(se->name == "Skyrim SE 1.5.97");
  check(ae->name == "Skyrim AE 1.6.1170");
  check(runtime_profile_for({1, 5, 96, 0}) == nullptr);
  check(runtime_profile_for({1, 6, 640, 0}) == nullptr);
  check(runtime_profile_for({1, 6, 1130, 0}) == nullptr);
  check(runtime_profile_for({1, 6, 1170, 1}) == nullptr);
  check(runtime_profile_for({1, 6, 1179, 0}) == nullptr);

  if (se == nullptr || ae == nullptr) {
    return failures + 1;
  }

  check(se->state.dynamic_width_scale == 0xFC);
  check(se->state.dynamic_height_scale == 0x100);
  check(se->state.previous_width_scale == 0x104);
  check(se->state.previous_height_scale == 0x108);
  check(se->state.dynamic_resolution_lock == 0x110);
  check(ae->state.dynamic_width_scale == 0x104);
  check(ae->state.dynamic_height_scale == 0x108);
  check(ae->state.previous_width_scale == 0x10C);
  check(ae->state.previous_height_scale == 0x110);
  check(ae->state.dynamic_resolution_lock == 0x118);
  check(se->state.projection_scale_x == 0x44);
  check(se->state.projection_scale_y == 0x48);
  check(se->state.frame_count == 0x4C);
  check(ae->state.projection_scale_x == 0x44);
  check(ae->state.projection_scale_y == 0x48);
  check(ae->state.frame_count == 0x4C);
  check(se->state.image_space_runtime_state == 0x1F0);
  check(ae->state.image_space_runtime_state == 0x1F0);
  check(se->state.runtime_taa_enabled == 0x18);
  check(ae->state.runtime_taa_enabled == 0x18);
  check(se->camera.near_plane == 0x40);
  check(se->camera.far_plane == 0x44);
  check(ae->camera.near_plane == 0x40);
  check(ae->camera.far_plane == 0x44);
  check(se->camera.frame_buffer_bytes == 720);
  check(ae->camera.frame_buffer_bytes == 720);

  check(se->address_ids.graphics_state == 524998);
  check(se->address_ids.image_space_state == 527731);
  check(se->address_ids.update_camera_data == 75472);
  check(se->address_ids.per_frame_buffer == 524768);
  check(se->address_ids.camera_parameters == 517032);
  check(se->address_ids.set_dirty_states == 75580);
  check(se->address_ids.dynamic_resolution_update == 35556);
  check(se->address_ids.temporal_jitter_update == 75460);
  check(se->address_ids.main_world_draw == 79947);
  check(se->address_ids.post_processing == 100430);
  check(ae->address_ids.graphics_state == 411479);
  check(ae->address_ids.image_space_state == 414660);
  check(ae->address_ids.update_camera_data == 77258);
  check(ae->address_ids.per_frame_buffer == 411384);
  check(ae->address_ids.camera_parameters == 403540);
  check(ae->address_ids.set_dirty_states == 77386);
  check(ae->address_ids.dynamic_resolution_update == 36555);
  check(ae->address_ids.temporal_jitter_update == 77245);
  check(ae->address_ids.main_world_draw == 82084);
  check(ae->address_ids.post_processing == 107148);

  check(se->hooks.resolution_call == 0x2D);
  check(se->hooks.jitter_call == 0xE5);
  check(se->hooks.main_draw_call == 0x16F);
  check(se->hooks.post_processing_call == 0x1F0);
  check(ae->hooks.resolution_call == 0x2D);
  check(ae->hooks.jitter_call == 0xE2);
  check(ae->hooks.main_draw_call == 0x17A);
  check(ae->hooks.post_processing_call == 0x1E7);
  constexpr std::uint8_t se_resolution_call_bytes[]{
      0xE9, 0x48, 0x8D, 0x0D, 0x43, 0xB8, 0xA7, 0x02,
      0xE8, 0xEE, 0xBD, 0x7C, 0x00, 0x48, 0x8D, 0x0D,
      0x37, 0x74, 0xA7, 0x02, 0xE8, 0x22, 0x90, 0x7B,
  };
  constexpr std::uint8_t se_jitter_call_bytes[]{
      0x02, 0xC6, 0x05, 0x3B, 0x27, 0x2C, 0x02, 0x01,
      0xE8, 0x06, 0x2E, 0x01, 0x00, 0x48, 0x8B, 0x0D,
      0xEF, 0xDC, 0x2B, 0x02, 0x48, 0x8B, 0x01, 0xFF,
  };
  constexpr std::uint8_t se_main_draw_call_bytes[]{
      0xD2, 0x48, 0x8D, 0x0D, 0x11, 0x8E, 0x16, 0x02,
      0xE8, 0xAC, 0xAC, 0xEA, 0xFF, 0x48, 0x8B, 0x05,
      0x15, 0x61, 0x06, 0x02, 0x48, 0x8B, 0x48, 0x10,
  };
  constexpr std::uint8_t se_post_processing_call_bytes[]{
      0x00, 0x48, 0x8B, 0x0D, 0x10, 0xC6, 0xEE, 0x01,
      0xE8, 0x7B, 0x28, 0xFB, 0xFF, 0x48, 0x8B, 0x05,
      0x1C, 0xE9, 0xF4, 0x01, 0x48, 0x8B, 0x88, 0x28,
  };
  constexpr std::uint8_t ae_resolution_call_bytes[]{
      0xE9, 0x48, 0x8D, 0x0D, 0xF3, 0x8F, 0xC4, 0x02,
      0xE8, 0xBE, 0x4B, 0x81, 0x00, 0x48, 0x8D, 0x0D,
      0x87, 0x4B, 0xC4, 0x02, 0xE8, 0x12, 0x09, 0x80,
  };
  constexpr std::uint8_t ae_jitter_call_bytes[]{
      0x02, 0xC6, 0x05, 0x02, 0x86, 0x44, 0x02, 0x01,
      0xE8, 0x99, 0x43, 0x01, 0x00, 0x48, 0x8B, 0x0D,
      0x32, 0x41, 0x44, 0x02, 0x48, 0x8B, 0x01, 0xFF,
  };
  constexpr std::uint8_t ae_main_draw_call_bytes[]{
      0xD2, 0x48, 0x8D, 0x0D, 0x46, 0x37, 0x2E, 0x02,
      0xE8, 0xD1, 0xF7, 0xE9, 0xFF, 0x48, 0x8B, 0x05,
      0x42, 0xC1, 0x64, 0x02, 0x48, 0x8B, 0x48, 0x10,
  };
  constexpr std::uint8_t ae_post_processing_call_bytes[]{
      0x00, 0x48, 0x8B, 0x0D, 0x29, 0xBD, 0xE5, 0x01,
      0xE8, 0x04, 0x32, 0xFB, 0xFF, 0x48, 0x8B, 0x05,
      0x65, 0xDF, 0xEB, 0x01, 0x48, 0x8B, 0x88, 0x28,
  };
  const auto check_approved_call =
      [&check](const CodeSignature &signature, const auto &expected,
               const std::uintptr_t expected_target_rva) {
        check(signature.relative_offset == -8);
        check(signature.length == std::size(expected));
        check(signature.expected_target_rva == expected_target_rva);
        check(signature.approved);
        for (std::size_t index = 0; index < std::size(expected); ++index) {
          check(signature.bytes[index] == expected[index]);
          check(signature.mask[index] == 0xFF);
        }
      };
  check_approved_call(se->hooks.resolution_call_signature,
                      se_resolution_call_bytes, 0xD7CE40);
  check_approved_call(se->hooks.jitter_call_signature, se_jitter_call_bytes,
                      0xD7CFB0);
  check_approved_call(se->hooks.main_draw_call_signature,
                      se_main_draw_call_bytes, 0xD6A330);
  check_approved_call(se->hooks.post_processing_call_signature,
                      se_post_processing_call_bytes, 0x1297410);
  const auto check_call_signature_shape = [&check](const CodeSignature &signature) {
    check(signature.bytes[8] == 0xE8);
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(signature.bytes[9]) |
        (static_cast<std::uint32_t>(signature.bytes[10]) << 8U) |
        (static_cast<std::uint32_t>(signature.bytes[11]) << 16U) |
        (static_cast<std::uint32_t>(signature.bytes[12]) << 24U));
    check(displacement != 0);
  };
  check_call_signature_shape(se->hooks.resolution_call_signature);
  check_call_signature_shape(se->hooks.jitter_call_signature);
  check_call_signature_shape(se->hooks.main_draw_call_signature);
  check_call_signature_shape(se->hooks.post_processing_call_signature);
  check_call_signature_shape(ae->hooks.resolution_call_signature);
  check_call_signature_shape(ae->hooks.jitter_call_signature);
  check_call_signature_shape(ae->hooks.main_draw_call_signature);
  check_call_signature_shape(ae->hooks.post_processing_call_signature);
  check_approved_call(ae->hooks.resolution_call_signature,
                      ae_resolution_call_bytes, 0xE587F0);
  check_approved_call(ae->hooks.jitter_call_signature, ae_jitter_call_bytes,
                      0xE58A10);
  check_approved_call(ae->hooks.main_draw_call_signature,
                      ae_main_draw_call_bytes, 0xE44850);
  check_approved_call(ae->hooks.post_processing_call_signature,
                      ae_post_processing_call_bytes, 0x1481B80);

  check(se->scaleform.begin_display_slot == 12);
  check(se->scaleform.end_display_slot == 13);
  check(ae->scaleform.begin_display_slot == 12);
  check(ae->scaleform.end_display_slot == 13);
  check(se->scaleform.abi_validated);
  check(ae->scaleform.abi_validated);

  check(se->jitter_fold_patch_supported);
  check(ae->jitter_fold_patch_supported);

  check(se->address_ids.jitter_update_gate == 75709);
  check(se->address_ids.camera_state_build == 75711);
  check(ae->address_ids.jitter_update_gate == 77518);
  check(ae->address_ids.camera_state_build == 77520);
  check(se->address_ids.jitter_update_gate != ae->address_ids.jitter_update_gate);
  check(se->address_ids.camera_state_build != ae->address_ids.camera_state_build);
  check(se->address_ids.camera_state_build != se->address_ids.jitter_update_gate);
  check(ae->address_ids.camera_state_build != ae->address_ids.jitter_update_gate);

  check(se->hooks.jitter_update_patch == 0x0E);
  check(ae->hooks.jitter_update_patch == 0x11);
  check(se->hooks.camera_state_patch == 0x1D5);
  check(ae->hooks.camera_state_patch == 0x1D5);

  constexpr std::array<std::uint8_t, 6> se_jitter_update_patch_bytes{
      0x80, 0x7A, 0x18, 0x00, 0x74, 0x7A};
  constexpr std::array<std::uint8_t, 6> ae_jitter_update_patch_bytes{
      0x80, 0x7A, 0x18, 0x00, 0x74, 0x68};
  constexpr std::array<std::uint8_t, 10> camera_state_patch_bytes{
      0x80, 0x79, 0x18, 0x00, 0x0F, 0x84, 0xC0, 0x00, 0x00, 0x00};

  const auto check_patch_signature =
      [&](const CodeSignature &signature, const std::uint8_t *expected,
          const std::size_t expected_length) {
        check(signature.approved);
        check(signature.length == expected_length);
        check(signature.length <= kMaximumHookSignatureBytes);
        for (std::size_t index = 0; index < expected_length; ++index) {
          check(signature.bytes[index] == expected[index]);
          check(signature.mask[index] == 0xFF);
        }
      };

  check_patch_signature(se->hooks.jitter_update_patch_signature,
                        se_jitter_update_patch_bytes.data(),
                        se_jitter_update_patch_bytes.size());
  check_patch_signature(ae->hooks.jitter_update_patch_signature,
                        ae_jitter_update_patch_bytes.data(),
                        ae_jitter_update_patch_bytes.size());
  check_patch_signature(se->hooks.camera_state_patch_signature,
                        camera_state_patch_bytes.data(),
                        camera_state_patch_bytes.size());
  check_patch_signature(ae->hooks.camera_state_patch_signature,
                        camera_state_patch_bytes.data(),
                        camera_state_patch_bytes.size());

  check(se->hooks.jitter_update_patch_signature.bytes !=
        ae->hooks.jitter_update_patch_signature.bytes);

  const auto check_profile_is_complete = [&check](const RuntimeProfile *const p) {
    check(!p->name.empty());

    check(p->state.projection_scale_x != 0);
    check(p->state.projection_scale_y != 0);
    check(p->state.frame_count != 0);
    check(p->state.dynamic_width_scale != 0);
    check(p->state.dynamic_height_scale != 0);
    check(p->state.previous_width_scale != 0);
    check(p->state.previous_height_scale != 0);
    check(p->state.dynamic_resolution_lock != 0);
    check(p->state.image_space_runtime_state != 0);
    check(p->state.runtime_taa_enabled != 0);

    check(p->camera.near_plane != 0);
    check(p->camera.far_plane != 0);
    check(p->camera.frame_buffer_bytes != 0);

    check(p->address_ids.graphics_state != 0);
    check(p->address_ids.image_space_state != 0);
    check(p->address_ids.update_camera_data != 0);
    check(p->address_ids.per_frame_buffer != 0);
    check(p->address_ids.camera_parameters != 0);
    check(p->address_ids.set_dirty_states != 0);
    check(p->address_ids.dynamic_resolution_update != 0);
    check(p->address_ids.temporal_jitter_update != 0);
    check(p->address_ids.jitter_update_gate != 0);
    check(p->address_ids.camera_state_build != 0);
    check(p->address_ids.main_world_draw != 0);
    check(p->address_ids.post_processing != 0);

    check(p->hooks.resolution_call != 0);
    check(p->hooks.jitter_call != 0);
    check(p->hooks.main_draw_call != 0);
    check(p->hooks.post_processing_call != 0);
    check(p->hooks.jitter_update_patch != 0);
    check(p->hooks.camera_state_patch != 0);

    check(p->hooks.jitter_update_patch_signature.approved);
    check(p->hooks.jitter_update_patch_signature.length != 0);
    check(p->hooks.camera_state_patch_signature.approved);
    check(p->hooks.camera_state_patch_signature.length != 0);

    check(p->scaleform.begin_display_slot != 0);
    check(p->scaleform.end_display_slot != 0);
    check(p->scaleform.abi_validated);
  };

  check_profile_is_complete(se);
  check_profile_is_complete(ae);

  const auto shares_address = [](const AddressLibraryIds &a,
                                 const AddressLibraryIds &b) {
    return a.graphics_state == b.graphics_state ||
           a.image_space_state == b.image_space_state ||
           a.update_camera_data == b.update_camera_data ||
           a.per_frame_buffer == b.per_frame_buffer ||
           a.camera_parameters == b.camera_parameters ||
           a.set_dirty_states == b.set_dirty_states ||
           a.dynamic_resolution_update == b.dynamic_resolution_update ||
           a.temporal_jitter_update == b.temporal_jitter_update ||
           a.jitter_update_gate == b.jitter_update_gate ||
           a.camera_state_build == b.camera_state_build ||
           a.main_world_draw == b.main_world_draw ||
           a.post_processing == b.post_processing;
  };
  check(!shares_address(se->address_ids, ae->address_ids));

  return failures;
}
