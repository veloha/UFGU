#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace mfgdlss::render
{
constexpr std::size_t kMaximumHookSignatureBytes = 32;

struct CodeSignature final
{
    std::ptrdiff_t relative_offset{};
    std::size_t length{};
    std::array<std::uint8_t, kMaximumHookSignatureBytes> bytes{};
    std::array<std::uint8_t, kMaximumHookSignatureBytes> mask{};
    std::uintptr_t expected_target_rva{};
    bool approved{};
};

enum class HookValidationFailure : std::uint8_t
{
    none,
    unreadable,
    no_expected_target_recorded,
    not_executable,
    truncated_call,
    wrong_opcode,
    address_overflow,
    target_outside_module,
    target_not_executable,
    signature_unapproved,
    invalid_signature,
    signature_mismatch,
    unexpected_target,
};

[[nodiscard]] constexpr const char* hook_validation_failure_name(
    const HookValidationFailure failure) noexcept
{
    switch (failure) {
    case HookValidationFailure::none: return "none";
    case HookValidationFailure::unreadable: return "unreadable";
    case HookValidationFailure::no_expected_target_recorded:
        return "no-expected-target-recorded";
    case HookValidationFailure::not_executable: return "not-executable";
    case HookValidationFailure::truncated_call: return "truncated-call";
    case HookValidationFailure::wrong_opcode: return "wrong-opcode";
    case HookValidationFailure::address_overflow: return "address-overflow";
    case HookValidationFailure::target_outside_module:
        return "target-outside-module";
    case HookValidationFailure::target_not_executable:
        return "target-not-executable";
    case HookValidationFailure::signature_unapproved:
        return "signature-unapproved";
    case HookValidationFailure::invalid_signature: return "invalid-signature";
    case HookValidationFailure::signature_mismatch: return "signature-mismatch";
    case HookValidationFailure::unexpected_target: return "unexpected-target";
    }
    return "unknown";
}

struct HookValidationResult final
{
    HookValidationFailure failure{HookValidationFailure::none};
    std::uintptr_t target{};

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return failure == HookValidationFailure::none;
    }
};

[[nodiscard]] constexpr bool range_contains(
    const std::uintptr_t begin,
    const std::uintptr_t end,
    const std::uintptr_t address,
    const std::size_t size) noexcept
{
    return begin < end && address >= begin && address < end &&
           size <= end - address;
}

[[nodiscard]] constexpr HookValidationFailure validate_code_signature(
    const std::span<const std::uint8_t> observed,
    const CodeSignature& signature) noexcept
{
    if (!signature.approved) {
        return HookValidationFailure::signature_unapproved;
    }
    if (signature.length == 0 ||
        signature.length > kMaximumHookSignatureBytes ||
        observed.size() < signature.length) {
        return HookValidationFailure::invalid_signature;
    }
    for (std::size_t index = 0; index < signature.length; ++index) {
        if ((observed[index] & signature.mask[index]) !=
            (signature.bytes[index] & signature.mask[index])) {
            return HookValidationFailure::signature_mismatch;
        }
    }
    return HookValidationFailure::none;
}

[[nodiscard]] constexpr HookValidationResult validate_direct_call(
    const std::span<const std::uint8_t> call_bytes,
    const std::span<const std::uint8_t> signature_bytes,
    const std::uintptr_t call_address,
    const std::uintptr_t module_begin,
    const std::uintptr_t module_end,
    const bool call_readable,
    const bool call_executable,
    const bool target_executable,
    const CodeSignature& signature) noexcept
{
    if (!call_readable) {
        return {HookValidationFailure::unreadable, 0};
    }
    if (!call_executable) {
        return {HookValidationFailure::not_executable, 0};
    }
    if (call_bytes.size() < 5) {
        return {HookValidationFailure::truncated_call, 0};
    }
    if (call_bytes[0] != 0xE8U) {
        return {HookValidationFailure::wrong_opcode, 0};
    }
    if (call_address > std::numeric_limits<std::uintptr_t>::max() - 5U) {
        return {HookValidationFailure::address_overflow, 0};
    }

    const auto raw_displacement =
        static_cast<std::uint32_t>(call_bytes[1]) |
        (static_cast<std::uint32_t>(call_bytes[2]) << 8U) |
        (static_cast<std::uint32_t>(call_bytes[3]) << 16U) |
        (static_cast<std::uint32_t>(call_bytes[4]) << 24U);
    const auto displacement = raw_displacement <= 0x7FFFFFFFU ?
        static_cast<std::int64_t>(raw_displacement) :
        static_cast<std::int64_t>(raw_displacement) - 0x100000000LL;
    const auto next_instruction = call_address + 5U;

    std::uintptr_t target{};
    if (displacement >= 0) {
        const auto positive = static_cast<std::uintptr_t>(displacement);
        if (next_instruction >
            std::numeric_limits<std::uintptr_t>::max() - positive) {
            return {HookValidationFailure::address_overflow, 0};
        }
        target = next_instruction + positive;
    } else {
        const auto magnitude = static_cast<std::uintptr_t>(-displacement);
        if (magnitude > next_instruction) {
            return {HookValidationFailure::address_overflow, 0};
        }
        target = next_instruction - magnitude;
    }

    if (!range_contains(module_begin, module_end, target, 1)) {
        return {HookValidationFailure::target_outside_module, target};
    }
    if (!target_executable) {
        return {HookValidationFailure::target_not_executable, target};
    }

    const auto signature_result =
        validate_code_signature(signature_bytes, signature);
    if (signature_result != HookValidationFailure::none) {
        return {signature_result, target};
    }
    if (signature.expected_target_rva == 0U) {
        return {
            HookValidationFailure::no_expected_target_recorded, target};
    }
    if (signature.expected_target_rva > module_end - module_begin ||
        module_begin > std::numeric_limits<std::uintptr_t>::max() -
                           signature.expected_target_rva) {
        return {HookValidationFailure::invalid_signature, target};
    }
    if (target != module_begin + signature.expected_target_rva) {
        return {HookValidationFailure::unexpected_target, target};
    }
    return {HookValidationFailure::none, target};
}

[[nodiscard]] constexpr bool atomic_patch_allowed(
    const bool first_valid,
    const bool second_valid) noexcept
{
    return first_valid && second_valid;
}
}
