#include "render/DepthDiagnostics.hpp"

#include "render/DebugViewCore.hpp"

#include <d3d11.h>

#include <SKSE/SKSE.h>

#include <array>
#include <atomic>
#include <string>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;

constexpr auto kStageCount = static_cast<std::size_t>(DepthStage::count);

[[nodiscard]] std::string describe_statistics(const ProbeStatistics& stats)
{
    if (!stats.valid) {
        return "no result";
    }
    const auto sampled = stats.sampled_pixels == 0 ? 1U : stats.sampled_pixels;
    const auto far_percent =
        100.0 * static_cast<double>(stats.far_pixels) /
        static_cast<double>(sampled);
    std::string text =
        std::format(
            "{}x{} stride {}, {} sampled: min={:.6f} max={:.6f}, "
            "far/cleared={} ({:.2f}%), non-finite={}, out-of-range={}",
            stats.source_width,
            stats.source_height,
            stats.stride,
            stats.sampled_pixels,
            stats.minimum,
            stats.maximum,
            stats.far_pixels,
            far_percent,
            stats.nonfinite_pixels,
            stats.out_of_range_pixels);
    if (stats.bounds_empty()) {
        text += ", non-far bounds=EMPTY (no geometry anywhere)";
    } else {
        text += std::format(
            ", non-far bounds={},{}..{},{}",
            stats.bounds_left,
            stats.bounds_top,
            stats.bounds_right,
            stats.bounds_bottom);
    }
    text += ", histogram=";
    for (std::uint32_t bucket = 0; bucket < kProbeHistogramBuckets; ++bucket) {
        text += std::format(
            "{}{}", bucket == 0 ? "" : "/", stats.histogram[bucket]);
    }
    return text;
}

[[nodiscard]] bool entirely_cleared(const ProbeStatistics& stats) noexcept
{
    return stats.valid && stats.sampled_pixels != 0 &&
           stats.far_pixels == stats.sampled_pixels;
}
}

const char* describe(const DepthStage stage) noexcept
{
    switch (stage) {
    case DepthStage::engine_before_copy:
        return "1. Skyrim's kMAIN depth, before the production copy";
    case DepthStage::input_after_copy:
        return "2. the DLSS depth input, immediately after the copy";
    case DepthStage::input_before_evaluate:
        return "3. the DLSS depth input, immediately before evaluation";
    case DepthStage::input_after_evaluate:
        return "4. the DLSS depth input, immediately after evaluation";
    case DepthStage::count:
        break;
    }
    return "unknown stage";
}

struct DepthDiagnostics::State
{
    std::array<ResourceProbe, kStageCount> probes;
    std::array<ProbeStatistics, kStageCount> results{};
    std::array<ProbeStatus, kStageCount> statuses{};
    std::atomic_bool armed{};
    bool round_reported{};
    std::string verdict;
};

DepthDiagnostics::DepthDiagnostics() : state_(std::make_unique<State>())
{
    state_->statuses.fill(ProbeStatus::idle);
}

DepthDiagnostics::~DepthDiagnostics() = default;

DepthDiagnostics& DepthDiagnostics::instance() noexcept
{
    static DepthDiagnostics singleton;
    return singleton;
}

void DepthDiagnostics::arm() noexcept
{
    if (state_ == nullptr) {
        return;
    }
    for (auto& probe : state_->probes) {
        probe.arm();
    }
    state_->results.fill(ProbeStatistics{});
    state_->statuses.fill(ProbeStatus::idle);
    state_->round_reported = false;
    state_->verdict.clear();
    state_->armed.store(true, std::memory_order_relaxed);
    logger::info(
        "Depth validation armed: measuring Skyrim's depth before the copy, the "
        "DLSS input after the copy, and the same input either side of "
        "evaluation");
}

bool DepthDiagnostics::armed() const noexcept
{
    return state_ != nullptr &&
           state_->armed.load(std::memory_order_relaxed);
}

void DepthDiagnostics::observe(
    const DepthStage stage,
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const texture,
    const bool reversed_depth)
{

    if (!armed() || stage == DepthStage::count) {
        return;
    }
    auto& probe = state_->probes[static_cast<std::size_t>(stage)];
    if (!probe.armed()) {
        return;
    }

    ProbeParams params{};
    params.channel = ProbeChannel::depth;
    params.reversed_depth = reversed_depth;
    params.far_epsilon = 1.0e-6F;
    if (texture != nullptr) {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
        if (srv_format_for(
                description.Format, description.BindFlags, srv_format)) {
            params.far_epsilon = depth_far_epsilon_for(srv_format);
        }
    }
    state_->statuses[static_cast<std::size_t>(stage)] =
        probe.capture(device, context, texture, params);
}

void DepthDiagnostics::collect(ID3D11DeviceContext* const context)
{
    if (!armed()) {
        return;
    }
    auto all_settled = true;
    for (std::size_t index = 0; index < kStageCount; ++index) {
        auto& probe = state_->probes[index];
        const auto status = probe.collect(context);
        state_->statuses[index] = status;
        if (status == ProbeStatus::complete) {
            state_->results[index] = probe.statistics();
        } else if (status == ProbeStatus::pending ||
                   status == ProbeStatus::idle || probe.armed()) {
            all_settled = false;
        }
    }
    if (!all_settled || state_->round_reported) {
        return;
    }

    state_->round_reported = true;
    state_->armed.store(false, std::memory_order_relaxed);

    logger::warn("---- depth validation, one round ----");
    for (std::size_t index = 0; index < kStageCount; ++index) {
        const auto stage = static_cast<DepthStage>(index);
        logger::warn(
            "  {}: [{}] {}",
            describe(stage),
            describe(state_->statuses[index]),
            describe_statistics(state_->results[index]));
    }

    const auto& engine =
        state_->results[static_cast<std::size_t>(
            DepthStage::engine_before_copy)];
    const auto& after_copy =
        state_->results[static_cast<std::size_t>(
            DepthStage::input_after_copy)];
    const auto& before_evaluate =
        state_->results[static_cast<std::size_t>(
            DepthStage::input_before_evaluate)];
    const auto& after_evaluate =
        state_->results[static_cast<std::size_t>(
            DepthStage::input_after_evaluate)];

    if (!engine.valid || !after_copy.valid) {
        state_->verdict = "incomplete: not every stage produced a result";
    } else if (entirely_cleared(engine)) {
        state_->verdict =
            "Skyrim's own depth target is entirely cleared at the point it is "
            "read; the copy is not the defect";
    } else if (entirely_cleared(after_copy)) {
        state_->verdict =
            "Skyrim's depth has content but the DLSS input is entirely "
            "cleared: THE COPY IS DROPPING THE BUFFER";
    } else if (before_evaluate.valid &&
               entirely_cleared(before_evaluate)) {
        state_->verdict =
            "the copy delivered depth but the input is cleared again before "
            "evaluation: something between the two is overwriting it";
    } else if (after_evaluate.valid && entirely_cleared(after_evaluate) &&
               !entirely_cleared(before_evaluate)) {
        state_->verdict =
            "the input is valid at evaluation and cleared afterwards: "
            "evaluation or a later component writes through it";
    } else {
        state_->verdict =
            "depth reaches DLSS with real content at every measured stage";
    }
    logger::warn("  verdict: {}", state_->verdict);
    logger::warn("---- end depth validation ----");
}

const ProbeStatistics& DepthDiagnostics::statistics(
    const DepthStage stage) const noexcept
{
    static const ProbeStatistics empty{};
    if (state_ == nullptr || stage == DepthStage::count) {
        return empty;
    }
    return state_->results[static_cast<std::size_t>(stage)];
}

ProbeStatus DepthDiagnostics::status(const DepthStage stage) const noexcept
{
    if (state_ == nullptr || stage == DepthStage::count) {
        return ProbeStatus::idle;
    }
    return state_->statuses[static_cast<std::size_t>(stage)];
}

bool DepthDiagnostics::round_complete() const noexcept
{
    return state_ != nullptr && state_->round_reported;
}

const char* DepthDiagnostics::verdict() const noexcept
{
    if (state_ == nullptr || state_->verdict.empty()) {
        return nullptr;
    }
    return state_->verdict.c_str();
}

void DepthDiagnostics::shutdown() noexcept
{
    if (state_ == nullptr) {
        return;
    }
    for (auto& probe : state_->probes) {
        probe.shutdown();
    }
    state_->results.fill(ProbeStatistics{});
    state_->statuses.fill(ProbeStatus::idle);
    state_->armed.store(false, std::memory_order_relaxed);
    state_->round_reported = false;
    state_->verdict.clear();
}
}
