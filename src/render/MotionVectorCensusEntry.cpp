#include "render/MotionVectorCensus.hpp"

#include "config/Settings.hpp"
#include "render/D3D12Backend.hpp"
#include "render/SharedResources.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <SKSE/SKSE.h>

#include <cmath>
#include <cstring>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;

constexpr std::uint32_t kSampleStride = 8U;
constexpr std::uint64_t kPresentsBetweenSamples = 3000ULL;
constexpr float kDepthScale = 65536.0F;

enum class CensusOutcome
{
    failed,
    unwritten,
    decided
};

constexpr std::uint32_t kMaxAttempts = 16U;
constexpr std::uint32_t kDecidedReportsWanted = 4U;

struct CensusState final
{
    std::uint64_t presents{};
    std::uint32_t attempts{};
    std::uint32_t decided_reports{};
    bool dilated_decided{};
    bool undilated_decided{};
    bool sample_undilated_next{};
    bool reported{};
    bool unavailable_logged{};
};

CensusState g_state;

CensusOutcome census_one(
    ID3D12Device* const device,
    ID3D12CommandQueue* const queue,
    ID3D12Resource* const motion,
    ID3D12Resource* const depth,
    const char* const label,
    const char* const consumer)
{
    if (motion == nullptr) {
        logger::info(
            "Motion vector census skipped the {} buffer because it does not "
            "exist on this path",
            label);
        return CensusOutcome::failed;
    }

    const auto description = motion->GetDesc();
    const auto width = static_cast<std::uint32_t>(description.Width);
    const auto height = description.Height;

    std::uint32_t results[16]{};
    if (!run_motion_vector_reduction(
            device,
            queue,
            motion,
            depth,
            width,
            height,
            static_cast<std::uint32_t>(description.Format),
            results)) {
        return CensusOutcome::failed;
    }

    const auto samples = results[2];
    if (samples == 0U) {
        logger::warn(
            "Motion vector census sampled nothing from the {} buffer at "
            "{}x{}. The dispatch ran and no thread passed the extent test, "
            "which means the extent handed to it does not describe the "
            "texture",
            label,
            width,
            height);
        return CensusOutcome::failed;
    }

    float peak_value = 0.0F;
    const auto peak_bits = results[0];
    std::memcpy(&peak_value, &peak_bits, sizeof(peak_value));
    const auto peak = static_cast<double>(peak_value);
    const auto non_finite = results[5];
    const auto zero_fraction =
        100.0 * static_cast<double>(results[1]) /
        static_cast<double>(samples);
    const auto depth_fraction =
        100.0 * static_cast<double>(results[3]) /
        static_cast<double>(samples);
    const auto depth_peak =
        static_cast<double>(results[4]) / static_cast<double>(kDepthScale);
    const auto diagonal = std::sqrt(
        static_cast<double>(width) * static_cast<double>(width) +
        static_cast<double>(height) * static_cast<double>(height));

    const auto* verdict =
        non_finite == samples ?
            "EVERY sampled texel is infinite or NaN. A real motion vector "
            "field cannot look like this, so this buffer is UNWRITTEN on the "
            "current path rather than badly encoded. Compare it against the "
            "other buffer in this pair before drawing any conclusion" :
        non_finite != 0U ?
            "NOT DECIDABLE, because some texels are infinite or NaN. A single "
            "non-finite vector poisons any interpolation that reads it, so "
            "that is the finding here, not the encoding" :
        peak <= 0.0 ? "not decidable from a motionless sample" :
        peak > diagonal ?
            "IMPOSSIBLE as a displacement, because the peak exceeds the frame "
            "diagonal. Neither encoding explains it, so the buffer or the "
            "extent handed to this census is wrong" :
        peak <= 1.0 ?
            "consistent with UV encoding, where a full-screen sweep is about "
            "1.0 and normal motion is a few hundredths" :
            "consistent with PIXEL encoding, which for a buffer UFGU has "
            "already converted means our own UV to pixel conversion ran";

    logger::info(
        "Motion vector census of the {} buffer, which {} reads: {} samples at "
        "{}x{} (every {}th texel), format {}. EXACT peak {:.6f} against a "
        "frame diagonal of {:.1f}, {:.2f}% of vectors exactly zero, {} "
        "non-finite. Distribution by magnitude, which cannot saturate the way "
        "a single mean can: <=0.001 {}, <=0.01 {}, <=0.1 {}, <=1 {}, <=10 {}, "
        "<=100 {}, <=1000 {}, >1000 {}. Depth cross-check: {:.2f}% of samples "
        "non-zero, peak {:.6f}. Verdict: {}",
        label,
        consumer,
        samples,
        width,
        height,
        kSampleStride,
        static_cast<std::uint32_t>(description.Format),
        peak,
        diagonal,
        zero_fraction,
        non_finite,
        results[6],
        results[7],
        results[8],
        results[9],
        results[10],
        results[11],
        results[12],
        results[13],
        depth_fraction,
        depth_peak,
        verdict);
    return non_finite == samples ? CensusOutcome::unwritten
                                 : CensusOutcome::decided;
}
}

void sample_motion_vector_census()
{
    if (!config::Settings::instance().experimental_features()) {
        return;
    }
    if (g_state.reported) {
        return;
    }
    ++g_state.presents;
    if (g_state.presents < kPresentsBetweenSamples) {
        return;
    }

    auto& backend = D3D12Backend::instance();
    auto* const device = static_cast<ID3D12Device*>(backend.native_device());
    auto* const queue =
        static_cast<ID3D12CommandQueue*>(backend.command_queue());
    auto& resources = SharedResources::instance();
    auto* const vendor =
        static_cast<ID3D12Resource*>(resources.vendor_motion_vectors());
    auto* const upscaling =
        static_cast<ID3D12Resource*>(resources.upscaling_motion_vectors());
    auto* const depth = static_cast<ID3D12Resource*>(resources.depth());
    if (device == nullptr || queue == nullptr || depth == nullptr ||
        (vendor == nullptr && upscaling == nullptr) ||
        !resources.temporal_inputs_prepared()) {
        if (!g_state.unavailable_logged) {
            g_state.unavailable_logged = true;
            logger::info(
                "Motion vector census is waiting for prepared temporal "
                "inputs. It samples the shared motion and depth textures on "
                "the D3D12 side, which only exist once a gameplay frame has "
                "prepared them");
        }
        return;
    }

    const auto take_undilated = g_state.sample_undilated_next;
    g_state.sample_undilated_next = !g_state.sample_undilated_next;

    auto* const texture = take_undilated ? vendor : upscaling;
    const auto* const label =
        take_undilated ? "vendor undilated" : "DLSS dilated";
    const auto* const consumer = take_undilated ?
        "the FidelityFX and XeSS frame generation path" :
        "the Streamline upscaling and DLSS-G path";

    ++g_state.attempts;
    const auto outcome =
        census_one(device, queue, texture, depth, label, consumer);

    if (outcome == CensusOutcome::decided) {
        ++g_state.decided_reports;
        if (take_undilated) {
            g_state.undilated_decided = true;
        } else {
            g_state.dilated_decided = true;
        }
    }

    if (g_state.dilated_decided && g_state.undilated_decided &&
        g_state.decided_reports >= kDecidedReportsWanted) {
        logger::info(
            "Motion vector census is complete. Both shared motion buffers "
            "were sampled on SEPARATE frames on purpose. Two blocking GPU "
            "submits inside one present broke the HUD-less colour capture and "
            "suspended frame generation for the rest of the session, measured "
            "on 2026-08-20 across four builds, so this census costs one "
            "dispatch per frame and never two");
        g_state.reported = true;
        return;
    }

    if (g_state.attempts >= kMaxAttempts) {
        logger::warn(
            "Motion vector census gave up after {} attempts. Decided so far: "
            "dilated {}, undilated {}. An entirely non-finite read is what an "
            "unwritten buffer looks like, so either no gameplay frame wrote "
            "it or the census is reading the wrong resource",
            g_state.attempts,
            g_state.dilated_decided,
            g_state.undilated_decided);
        g_state.reported = true;
        return;
    }

    g_state.presents = 0ULL;
}
}
