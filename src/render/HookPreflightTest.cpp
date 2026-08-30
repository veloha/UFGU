#include "render/HookPreflight.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace
{
using mfgdlss::render::CodeSignature;
using mfgdlss::render::HookValidationFailure;

constexpr CodeSignature approved_call_signature()
{
    CodeSignature signature{};
    signature.length = 6;
    signature.bytes = {0x48, 0x8B, 0xE8, 0, 0, 0, 0};
    signature.mask = {0xFF, 0xFF, 0xFF, 0, 0, 0};
    signature.expected_target_rva = 0x80;
    signature.approved = true;
    return signature;
}
}

int main()
{

    using namespace mfgdlss::render;
    int failures{};
    const auto check = [&failures](const bool condition) {
        failures += condition ? 0 : 1;
    };

    constexpr std::uintptr_t module_begin = 0x1000;
    constexpr std::uintptr_t module_end = 0x2000;
    constexpr std::uintptr_t call_address = 0x1100;

    constexpr std::array<std::uint8_t, 5> call{0xE8, 0x7B, 0, 0, 0};
    constexpr std::array<std::uint8_t, 6> neighbourhood{
        0x48, 0x8B, 0xE8, 0x7B, 0, 0};

    auto signature = approved_call_signature();
    signature.expected_target_rva = 0x180;
    auto result = validate_direct_call(
        call,
        neighbourhood,
        call_address,
        module_begin,
        module_end,
        true,
        true,
        true,
        signature);
    check(result.valid());
    check(result.target == 0x1180);

    auto wrong_opcode = call;
    wrong_opcode[0] = 0xE9;
    result = validate_direct_call(
        wrong_opcode,
        neighbourhood,
        call_address,
        module_begin,
        module_end,
        true,
        true,
        true,
        signature);
    check(result.failure == HookValidationFailure::wrong_opcode);

    result = validate_direct_call(
        std::span{call}.first<4>(),
        neighbourhood,
        call_address,
        module_begin,
        module_end,
        true,
        true,
        true,
        signature);
    check(result.failure == HookValidationFailure::truncated_call);

    auto unapproved = signature;
    unapproved.approved = false;
    result = validate_direct_call(
        call,
        neighbourhood,
        call_address,
        module_begin,
        module_end,
        true,
        true,
        true,
        unapproved);
    check(result.failure == HookValidationFailure::signature_unapproved);

    auto bad_neighbourhood = neighbourhood;
    bad_neighbourhood[1] = 0x90;
    result = validate_direct_call(
        call,
        bad_neighbourhood,
        call_address,
        module_begin,
        module_end,
        true,
        true,
        true,
        signature);
    check(result.failure == HookValidationFailure::signature_mismatch);

    auto wrong_target = signature;
    wrong_target.expected_target_rva = 0x181;
    result = validate_direct_call(
        call,
        neighbourhood,
        call_address,
        module_begin,
        module_end,
        true,
        true,
        true,
        wrong_target);
    check(result.failure == HookValidationFailure::unexpected_target);

    constexpr std::array<std::uint8_t, 5> outside_call{
        0xE8, 0xFB, 0x1E, 0, 0};
    result = validate_direct_call(
        outside_call,
        neighbourhood,
        call_address,
        module_begin,
        module_end,
        true,
        true,
        true,
        signature);
    check(result.failure == HookValidationFailure::target_outside_module);

    result = validate_direct_call(
        call,
        neighbourhood,
        call_address,
        module_begin,
        module_end,
        true,
        true,
        false,
        signature);
    check(result.failure == HookValidationFailure::target_not_executable);

    check(atomic_patch_allowed(true, true));
    check(!atomic_patch_allowed(true, false));
    check(!atomic_patch_allowed(false, true));
    check(!atomic_patch_allowed(false, false));

    constexpr std::uintptr_t begin = 0x1000;
    constexpr std::uintptr_t end = 0x2000;
    check(range_contains(begin, end, 0x1000, 1));
    check(range_contains(begin, end, 0x1FFF, 1));
    check(range_contains(begin, end, 0x1F00, 0x100));
    check(range_contains(begin, end, 0x1000, 0));
    check(!range_contains(begin, end, 0x1F00, 0x101));
    check(!range_contains(begin, end, 0x1FFF, 2));
    check(!range_contains(begin, end, 0x2000, 1));
    check(!range_contains(begin, end, 0x0FFF, 1));
    check(!range_contains(begin, begin, 0x1000, 1));
    check(!range_contains(end, begin, 0x1500, 1));

    constexpr std::array<std::uint8_t, 6> exact{
        0x80, 0x7A, 0x18, 0x00, 0x74, 0x7A};
    constexpr auto full_mask_signature = [](const std::size_t length) {
        CodeSignature probe{};
        probe.length = length;
        probe.bytes = {0x80, 0x7A, 0x18, 0x00, 0x74, 0x7A, 0x00};
        probe.mask = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        probe.approved = true;
        return probe;
    };

    check(validate_code_signature(exact, full_mask_signature(6)) ==
          HookValidationFailure::none);

    {
        auto candidate = full_mask_signature(6);
        candidate.approved = false;
        check(validate_code_signature(exact, candidate) ==
              HookValidationFailure::signature_unapproved);
        candidate.length = 0;
        check(validate_code_signature(exact, candidate) ==
              HookValidationFailure::signature_unapproved);
    }

    check(validate_code_signature(exact, full_mask_signature(0)) ==
          HookValidationFailure::invalid_signature);
    check(validate_code_signature(
              exact, full_mask_signature(kMaximumHookSignatureBytes + 1)) ==
          HookValidationFailure::invalid_signature);
    check(validate_code_signature(
              std::span{exact}.first(5), full_mask_signature(6)) ==
          HookValidationFailure::invalid_signature);

    {
        auto observed = exact;
        observed[5] = 0x68;
        check(validate_code_signature(observed, full_mask_signature(6)) ==
              HookValidationFailure::signature_mismatch);

        auto candidate = full_mask_signature(6);
        candidate.mask[5] = 0x00;
        check(validate_code_signature(observed, candidate) ==
              HookValidationFailure::none);
        candidate.mask[5] = 0xF0;
        check(validate_code_signature(observed, candidate) ==
              HookValidationFailure::signature_mismatch);
        candidate.mask[5] = 0x0F;
        check(validate_code_signature(observed, candidate) ==
              HookValidationFailure::signature_mismatch);

        auto same_high_nibble = exact;
        same_high_nibble[5] = 0x7C;
        candidate.mask[5] = 0xF0;
        check(validate_code_signature(same_high_nibble, candidate) ==
              HookValidationFailure::none);
        candidate.mask[5] = 0x0F;
        check(validate_code_signature(same_high_nibble, candidate) ==
              HookValidationFailure::signature_mismatch);
    }

    {
        constexpr std::array<std::uint8_t, 8> longer{
            0x80, 0x7A, 0x18, 0x00, 0x74, 0x7A, 0xCC, 0xCC};
        check(validate_code_signature(longer, full_mask_signature(6)) ==
              HookValidationFailure::none);
    }

    static_assert(
        validate_code_signature(exact, full_mask_signature(6)) ==
            HookValidationFailure::none,
        "an exact match against an approved signature must validate");

    return failures;
}
