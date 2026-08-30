#include "render/DebugViewPass.hpp"

#include "config/Settings.hpp"
#include "render/CameraData.hpp"
#include "render/DynamicResolution.hpp"
#include "render/PresentationBridge.hpp"
#include "render/SharedResources.hpp"
#include "render/UpscalingPass.hpp"
#include "streamline/SuperResolution.hpp"

#include <d3d11.h>

#include <SKSE/SKSE.h>

#include <atomic>
#include <mutex>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;

std::atomic<std::uint32_t> g_selected_view{
    static_cast<std::uint32_t>(DebugView::off)};
std::once_flag g_startup_default_once;

void apply_startup_default()
{
    std::call_once(g_startup_default_once, []() {
        const auto raw = config::Settings::instance().debug_view_index();
        const auto view = debug_view_from_index(raw);

        g_selected_view.store(
            static_cast<std::uint32_t>(view), std::memory_order_relaxed);
        logger::info(
            "Debug view startup state initialized to {}",
            static_cast<unsigned>(view));
    });
}

void forward_to_log(const char* const message)
{
    logger::info("{}", message);
}
}

DebugViewPass::DebugViewPass()
{
    set_debug_view_log_sink(&forward_to_log);
}

DebugViewPass::~DebugViewPass() = default;

DebugViewPass& DebugViewPass::instance() noexcept
{
    static DebugViewPass pass;
    return pass;
}

bool DebugViewPass::armed() noexcept
{

    apply_startup_default();
    return g_selected_view.load(std::memory_order_relaxed) !=
        static_cast<std::uint32_t>(DebugView::off);
}

DebugView DebugViewPass::selected_view() noexcept
{
    apply_startup_default();
    return debug_view_from_index(
        g_selected_view.load(std::memory_order_relaxed));
}

void DebugViewPass::select(const DebugView view) noexcept
{

    apply_startup_default();
    g_selected_view.store(
        static_cast<std::uint32_t>(view), std::memory_order_relaxed);
    logger::info(
        "Debug view selection published: {}",
        static_cast<unsigned>(view));

    DynamicResolution::instance().arm_extent_observation(
        view != DebugView::off);
    instance().frames_since_select_ = 0;
    instance().next_motion_capture_frame_ = 0;
}

DebugViewStatus DebugViewPass::last_status() const noexcept
{
    return last_status_;
}

const char* DebugViewPass::unavailable_reason() const noexcept
{
    return last_status_ == DebugViewStatus::drawn ?
        nullptr :
        describe(last_status_);
}

DebugViewPass::SourceInfo DebugViewPass::source_info() const noexcept
{
    return source_info_;
}

const ProbeStatistics& DebugViewPass::motion_statistics() const noexcept
{
    return motion_probe_.statistics();
}

std::uint64_t DebugViewPass::frames_since_select() const noexcept
{
    return frames_since_select_;
}

void DebugViewPass::shutdown() noexcept
{
    renderer_.shutdown();
    motion_probe_.shutdown();
    raw_motion_probe_.shutdown();
    last_status_ = DebugViewStatus::off;
    source_info_ = {};
    logged_status_ = DebugViewStatus::off;
    frames_since_select_ = 0;
    next_motion_capture_frame_ = 0;
}

bool DebugViewPass::render(ID3D11Texture2D* const target)
{

    const auto view = selected_view();
    if (view == DebugView::off) {
        if (renderer_.holds_resources() || motion_probe_.holds_resources() ||
            raw_motion_probe_.holds_resources()) {

            renderer_.shutdown();
            motion_probe_.shutdown();
            raw_motion_probe_.shutdown();
            logger::info(
                "Debug view returned to Off; every cached device object was "
                "released and the production image is untouched");
        }
        last_status_ = DebugViewStatus::off;
        source_info_ = {};
        logged_status_ = DebugViewStatus::off;
        frames_since_select_ = 0;
        next_motion_capture_frame_ = 0;
        return false;
    }

    auto& presentation = PresentationBridge::instance();
    auto* device = presentation.d3d11_device();
    auto* context = presentation.d3d11_context();

    auto& upscaling = UpscalingPass::instance();
    ID3D11Texture2D* source{};
    switch (view) {
    case DebugView::motion_vectors:
        source = upscaling.debug_motion_source();
        break;
    case DebugView::generator_motion:
        source = upscaling.debug_generator_motion_source();
        break;
    case DebugView::depth:
        source = upscaling.debug_depth_source();
        break;
    case DebugView::generator_depth:
        source = SharedResources::instance().depth_d3d11();
        break;
    case DebugView::ui_layer:
        source = SharedResources::instance().ui_color_alpha_d3d11();
        break;
    case DebugView::scene_reference:
    case DebugView::violations:
        source = upscaling.debug_color_source();
        break;
    case DebugView::off:
        return false;
    }

    const auto& super_resolution = streamline::SuperResolution::instance();
    auto& dynamic_resolution = DynamicResolution::instance();

    DebugViewParams params{};
    params.view = view;
    params.active_width = super_resolution.render_width();
    params.active_height = super_resolution.render_height();

    params.reversed_depth =
        CameraData::instance().depth_reversed_for_display();
    params.frame_tainted = dynamic_resolution.extent_violation_observed();

    ++frames_since_select_;

    if (view == DebugView::motion_vectors && source != nullptr) {

        if (!motion_probe_.armed() &&
            frames_since_select_ >= next_motion_capture_frame_) {
            next_motion_capture_frame_ =
                frames_since_select_ + kMotionCaptureIntervalFrames;
            motion_probe_.arm();
            raw_motion_probe_.arm();
        }
        ProbeParams motion_params{};
        motion_params.channel = ProbeChannel::motion;

        motion_params.near_zero_threshold =
            params.active_width != 0 ?
                0.01F / static_cast<float>(params.active_width) :
                1.0e-5F;
        motion_params.maximum_expected_magnitude =
            kMotionHistogramScale;
        static_cast<void>(motion_probe_.capture(
            device, context, source, motion_params));
        static_cast<void>(raw_motion_probe_.capture(
            device, context, upscaling.debug_raw_motion_source(),
            motion_params));
    } else {
        static_cast<void>(motion_probe_.collect(context));
        static_cast<void>(raw_motion_probe_.collect(context));
    }

    const auto& raw_motion = raw_motion_probe_.statistics();
    if (const auto& motion = motion_probe_.statistics();
        motion.valid && motion.sampled_pixels != 0U &&
        motion_statistics_logged_ < kMotionStatisticsLogBudget &&
        (motion.maximum != last_logged_motion_maximum_ ||
         raw_motion.maximum != last_logged_raw_motion_maximum_)) {
        ++motion_statistics_logged_;
        last_logged_motion_maximum_ = motion.maximum;
        last_logged_raw_motion_maximum_ = raw_motion.maximum;
        logger::info(
            "Motion probe. Skyrim's own target: {} texels of {}x{}, magnitude "
            "min={:.8f} max={:.8f}, non-finite={}, valid={}. What the upscaler "
            "consumes: {} texels of {}x{}, magnitude min={:.8f} max={:.8f}, "
            "non-finite={}. Normalised screen space, so 1.0 is a full screen "
            "of movement, and the histogram saturates at {:.2f}. A fast camera "
            "pan measured 0.121 on 2026-08-20, so a scale of 0.05 put most of "
            "the frame in the top bucket exactly when motion matters most.",
            raw_motion.sampled_pixels,
            raw_motion.source_width,
            raw_motion.source_height,
            raw_motion.minimum,
            raw_motion.maximum,
            raw_motion.nonfinite_pixels,
            raw_motion.valid,
            motion.sampled_pixels,
            motion.source_width,
            motion.source_height,
            motion.minimum,
            motion.maximum,
            motion.nonfinite_pixels,
            kMotionHistogramScale);
    }

    const auto status = renderer_.render(
        device, context, target, source, params);
    last_status_ = status;

    if (status != DebugViewStatus::drawn) {
        source_info_ = {};
        if (logged_status_ != status || logged_view_ != view) {
            logged_status_ = status;
            logged_view_ = view;
            logger::warn(
                "Debug view {} is unavailable: {}",
                static_cast<unsigned>(view),
                describe(status));
        }
        return false;
    }
    if (logged_status_ != status || logged_view_ != view) {
        logger::info(
            "Debug view {} rendered successfully on frame {} after "
            "selection",
            static_cast<unsigned>(view),
            frames_since_select_);
    }
    logged_status_ = status;
    logged_view_ = view;

    D3D11_TEXTURE2D_DESC source_description{};
    source->GetDesc(&source_description);
    D3D11_TEXTURE2D_DESC target_description{};
    target->GetDesc(&target_description);

    source_info_.source_width = source_description.Width;
    source_info_.source_height = source_description.Height;
    source_info_.source_format =
        static_cast<std::uint32_t>(source_description.Format);
    source_info_.sampled_format = static_cast<std::uint32_t>(
        renderer_.resolved_source_view_format());
    source_info_.active_width = params.active_width;
    source_info_.active_height = params.active_height;
    source_info_.output_width = target_description.Width;
    source_info_.output_height = target_description.Height;

    source_info_.motion_scale_x = static_cast<float>(params.active_width);
    source_info_.motion_scale_y = static_cast<float>(params.active_height);
    source_info_.reversed_depth = params.reversed_depth;
    source_info_.valid = true;
    return true;
}
}
