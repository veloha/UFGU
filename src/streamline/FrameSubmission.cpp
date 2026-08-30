#include "streamline/FrameSubmission.hpp"

#include "render/CameraData.hpp"
#include "render/DynamicResolution.hpp"
#include "render/JitterOwnership.hpp"
#include "render/SharedResources.hpp"
#include "render/UpscalingPass.hpp"
#include "streamline/FrameTagPolicy.hpp"
#include "streamline/StreamlineApi.hpp"
#include "streamline/SuperResolution.hpp"

#include <d3d12.h>

#include <SKSE/SKSE.h>

#include <sl_consts.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <array>

namespace mfgdlss::streamline
{
namespace
{
namespace logger = SKSE::log;

const sl::ViewportHandle kMainViewport{0};

render::BoundedLogBudget submission_gate_budget{6U};
render::BoundedLogBudget submission_silent_budget{24U};

std::array<std::uint64_t, 8> g_submit_failure_counts{};

[[nodiscard]] bool report_submit_failure(const std::size_t path) noexcept
{
    if (path >= g_submit_failure_counts.size()) {
        return false;
    }
    const auto count = ++g_submit_failure_counts[path];
    return count == 1ULL || count == 10ULL || count == 100ULL ||
           count == 1000ULL || count % 5000ULL == 0ULL;
}

[[nodiscard]] std::uint64_t submit_failure_count(
    const std::size_t path) noexcept
{
    return path < g_submit_failure_counts.size() ?
        g_submit_failure_counts[path] : 0ULL;
}
render::BoundedLogBudget constants_gate_budget{6U};

[[nodiscard]] sl::Resource make_resource(void* native)
{
    return {
        sl::ResourceType::eTex2d,
        native,
        D3D12_RESOURCE_STATE_COMMON};
}

template <class Function>
[[nodiscard]] Function* load_feature_function(
    const sl::Feature feature,
    const char* name)
{
    void* function{};
    const auto result =
        Api::instance().get_feature_function(feature, name, function);
    if (result != sl::Result::eOk || function == nullptr) {
        logger::error(
            "Unable to load Streamline timing function {}: {}",
            name,
            static_cast<int>(result));
        return nullptr;
    }
    return reinterpret_cast<Function*>(function);
}
}

FrameSubmission& FrameSubmission::instance() noexcept
{
    static FrameSubmission submission;
    return submission;
}

bool FrameSubmission::initialize_timing(
    const std::uint32_t output_width,
    const std::uint32_t output_height)
{
    if (timing_ready_) {
        if (output_width_ != output_width || output_height_ != output_height) {
            reset_pending_ = true;
        }
        output_width_ = output_width;
        output_height_ = output_height;
        return true;
    }

    reflex_sleep_ =
        load_feature_function<PFun_slReflexSleep>(
            sl::kFeatureReflex,
            "slReflexSleep");
    pcl_set_marker_ =
        load_feature_function<PFun_slPCLSetMarker>(
            sl::kFeaturePCL,
            "slPCLSetMarker");
    pcl_get_state_ =
        load_feature_function<PFun_slPCLGetState>(
            sl::kFeaturePCL,
            "slPCLGetState");
    pcl_set_options_ =
        load_feature_function<PFun_slPCLSetOptions>(
            sl::kFeaturePCL,
            "slPCLSetOptions");

    timing_ready_ = pcl_set_marker_ != nullptr;
    if (!timing_ready_) {
        return false;
    }

    output_width_ = output_width;
    output_height_ = output_height;

    if (pcl_set_options_ != nullptr) {
        sl::PCLOptions pcl_options{};
        pcl_options.virtualKey = sl::PCLHotKey::eUsePingMessage;
        pcl_options.idThread = GetCurrentThreadId();
        const auto options_result =
            reinterpret_cast<PFun_slPCLSetOptions*>(pcl_set_options_)(
                pcl_options);
        if (options_result != sl::Result::eOk) {
            logger::warn(
                "slPCLSetOptions failed: {}. PCL will not know which thread "
                "pumps messages for this process",
                static_cast<int>(options_result));
        }
    }
    if (pcl_get_state_ != nullptr) {
        sl::PCLState pcl_state{};
        const auto state_result =
            reinterpret_cast<PFun_slPCLGetState*>(pcl_get_state_)(pcl_state);
        if (state_result == sl::Result::eOk) {
            pcl_ping_message_ = pcl_state.statsWindowMessage;
        }
    }

    logger::info(
        "PC latency markers ready on this adapter. Reflex sleep {}. NVIDIA "
        "documents PCL Stats as supported on every GPU, vendor and driver, so "
        "the markers are no longer gated behind the Reflex sleep call and now "
        "run on AMD, Intel and NVIDIA cards without DLSS-G. PCL ping window "
        "message id {}. The ping marker itself needs a window message pump, "
        "which this plugin does not own, so input sampling latency is the one "
        "PCL component still missing",
        reflex_sleep_ != nullptr ? "is available" :
            "is NOT available on this adapter",
        pcl_ping_message_);
    return true;
}

bool FrameSubmission::acquire_frame_token()
{
    const auto result =
        Api::instance().get_new_frame_token(frame_token_, &frame_index_);
    if (result != sl::Result::eOk || frame_token_ == nullptr) {
        if (!failure_logged_) {
            logger::error(
                "slGetNewFrameToken failed: {}",
                static_cast<int>(result));
            failure_logged_ = true;
        }
        frame_prepared_ = false;
        return false;
    }
    frame_prepared_ = true;
    constants_submitted_ = false;
    return true;
}

void FrameSubmission::set_marker(const std::uint32_t marker)
{
    if (!timing_ready_ || !frame_prepared_ || frame_token_ == nullptr) {
        return;
    }
    const auto result =
        reinterpret_cast<PFun_slPCLSetMarker*>(pcl_set_marker_)(
            static_cast<sl::PCLMarker>(marker),
            *frame_token_);
    if (result != sl::Result::eOk && !failure_logged_) {
        logger::error(
            "slPCLSetMarker {} failed: {}",
            marker,
            static_cast<int>(result));
        failure_logged_ = true;
    }
}

bool FrameSubmission::begin_frame()
{
    ++begin_call_count_;
    if (!timing_ready_) {
        return false;
    }

    if (frame_open_) {
        ++duplicate_begin_count_;
        if (!duplicate_begin_logged_) {
            duplicate_begin_logged_ = true;
            logger::warn(
                "Repeated begin-frame callback before Present; reusing "
                "Streamline frame {}",
                frame_index_);
        }
        return false;
    }

    frame_open_ = true;
    ++opened_frame_count_;
    frame_index_ = presentation_index_;
    frame_token_ = nullptr;
    frame_prepared_ = false;
    constants_submitted_ = false;

    if (acquire_frame_token() &&
        reflex_sleep_enabled_ &&
        reflex_sleep_ != nullptr &&
        frame_token_ != nullptr) {
        const auto sleep_result =
            reinterpret_cast<PFun_slReflexSleep*>(
                reflex_sleep_)(*frame_token_);
        if (sleep_result != sl::Result::eOk && !failure_logged_) {
            logger::error(
                "slReflexSleep failed: {}",
                static_cast<int>(sleep_result));
            failure_logged_ = true;
        }
    }
    if (frame_prepared_) {
        set_marker(
            static_cast<std::uint32_t>(
                sl::PCLMarker::eSimulationStart));
        simulation_open_ = true;
    }
    return true;
}

void FrameSubmission::set_reflex_sleep_enabled(
    const bool enabled) noexcept
{
    reflex_sleep_enabled_ = enabled;
}

bool FrameSubmission::ensure_constants(
    const bool reset,
    const bool motion_vectors_dilated)
{
    if (!frame_open_) {
        if (!failure_logged_) {
            logger::error(
                "Streamline constants requested outside an open frame");
            failure_logged_ = true;
        }
        return false;
    }
    if (!frame_prepared_ && !acquire_frame_token()) {
        if (constants_gate_budget.admit(0x10U)) {
            logger::warn(
                "Streamline constants refused: no frame token could be "
                "acquired. Frame generation cannot be enabled without one.");
        }
        return false;
    }
    if (constants_submitted_) {
        return true;
    }

    sl::Constants constants{};
    const auto reset_this_frame = reset || reset_pending_;
    if (!render::CameraData::instance().build_constants(
            constants,
            reset_this_frame,
            motion_vectors_dilated)) {
        if (constants_gate_budget.admit(0x20U)) {
            logger::warn(
                "Streamline constants refused: CameraData::build_constants "
                "failed, so the per-frame camera/depth contract is not usable "
                "this frame. Frame generation stays at 1x.");
        }
        return false;
    }
    const auto result =
        Api::instance().set_constants(
            constants,
            *frame_token_,
            kMainViewport);
    if (result != sl::Result::eOk) {
        if (!failure_logged_) {
            logger::error(
                "slSetConstants failed: {}",
                static_cast<int>(result));
            failure_logged_ = true;
        }
        return false;
    }
    reset_pending_ = false;
    constants_submitted_ = true;
    const auto camera_sequence =
        render::CameraData::instance().capture_sequence();
    if (last_camera_sequence_ != 0 &&
        camera_sequence == last_camera_sequence_ &&
        !stale_camera_logged_) {
        stale_camera_logged_ = true;
        logger::warn(
            "Camera constants were reused across Streamline frames "
            "{} and {}; temporal inputs may be out of phase",
            frame_index_ - 1,
            frame_index_);
    }
    last_camera_sequence_ = camera_sequence;
    if (!lifecycle_logged_) {
        lifecycle_logged_ = true;
        logger::info(
            "Presentation-synchronized Streamline lifecycle active: "
            "frame={}, camera-capture={}",
            frame_index_,
            camera_sequence);
    }
    return true;
}

bool FrameSubmission::submit()
{
    auto& resources = render::SharedResources::instance();
    const auto resources_ready = resources.ready();
    const auto camera_ready = render::CameraData::instance().ready();
    if (!resources_ready || !camera_ready) {
        if (submission_gate_budget.admit(
                (resources_ready ? 0x1U : 0U) | (camera_ready ? 0x2U : 0U))) {
            logger::warn(
                "Streamline submission refused before tagging: "
                "SharedResources::ready={}, CameraData::ready={}. Frame "
                "generation cannot be enabled until both are true.",
                resources_ready,
                camera_ready);
        }
        return false;
    }

    const auto color_description =
        static_cast<ID3D12Resource*>(resources.color())->GetDesc();
    const sl::Extent render_extent{
        0,
        0,
        static_cast<std::uint32_t>(color_description.Width),
        color_description.Height};
    const sl::Extent output_extent{
        0,
        0,
        output_width_ != 0 ? output_width_ : render_extent.width,
        output_height_ != 0 ? output_height_ : render_extent.height};

    if (output_extent.width == 0 || output_extent.height == 0 ||
        render_extent.width == 0 || render_extent.height == 0) {
        if (!zero_extent_logged_) {
            zero_extent_logged_ = true;
            logger::warn(
                "MFG submission rejected: a tagged extent has a zero dimension "
                "(render {}x{}, output {}x{})",
                render_extent.width,
                render_extent.height,
                output_extent.width,
                output_extent.height);
        }
        return false;
    }
    zero_extent_logged_ = false;
    const auto& super_resolution = SuperResolution::instance();
    const auto reduced_mode =
        super_resolution.enabled() &&
        (super_resolution.render_width() < output_extent.width ||
         super_resolution.render_height() < output_extent.height);
    const auto reduced_input_verified =
        !reduced_mode ||
        (render::DynamicResolution::instance()
             .scene_viewport_verified() &&
         render::UpscalingPass::instance()
             .evaluation_verified());
    if (!reduced_input_verified) {
        if (report_submit_failure(4U)) {
            logger::warn(
                "MFG suspended: reduced DLSS mode has no verified reduced "
                "scene viewport");
        }
        return false;
    }
    reduced_viewport_rejection_logged_ = false;
    const sl::Extent temporal_extent{
        0,
        0,
        super_resolution.enabled() ?
            super_resolution.render_width() :
            render_extent.width,
        super_resolution.enabled() ?
            super_resolution.render_height() :
            render_extent.height};
    if (!resources.prepare_temporal_inputs(
            temporal_extent.width,
            temporal_extent.height,
            true)) {
        if (submission_silent_budget.admit(1U)) {
            logger::warn(
                "MFG submission stopped: prepare_temporal_inputs failed for a "
                "{}x{} temporal extent. Frame generation cannot be submitted "
                "this frame. This path returned silently before 2026-08-21, "
                "which is why a dropout here could never be diagnosed. If the "
                "deterministic gate is raised it will NOT release while this "
                "keeps failing, because the gate releases only when copy_frame "
                "and submit both succeed",
                temporal_extent.width,
                temporal_extent.height);
        }
        return false;
    }

    if (!ensure_constants(false, false)) {
        if (submission_silent_budget.admit(2U)) {
            logger::warn(
                "MFG submission stopped: ensure_constants failed, so the "
                "camera and projection constants Streamline needs were not "
                "published this frame. This path returned silently before "
                "2026-08-21. A raised deterministic gate will NOT release "
                "while this keeps failing");
        }
        return false;
    }

    close_simulation_marker();

    auto& api = Api::instance();

    const auto streamline_ui_recomposition =
        resources.streamline_ui_recomposition_available();
    const auto tag_policy = make_frame_tag_policy(
        streamline_ui_recomposition,
        reduced_mode);

    if (!tag_policy.submission_allowed) {
        if (report_submit_failure(5U)) {
            logger::warn(
                "MFG submission rejected for the {}th time: reduced-resolution "
                "mode expects a native HUD-less/UI separation and none is "
                "available for this frame. RECURRENCE MATTERS: a rising "
                "count here means frame generation is dead for as long as "
                "it keeps rising, because submit() returning false leaves "
                "the gate unable to release",
                submit_failure_count(5U));
        }
        return false;
    }
    reduced_ui_rejection_logged_ = false;
    auto color = make_resource(
        tag_policy.use_hudless_ui ?
            resources.frame_generation_hudless() : nullptr);
    auto* const motion_native =
        static_cast<ID3D12Resource*>(resources.motion_vectors());
    auto* const depth_native =
        static_cast<ID3D12Resource*>(resources.depth());
    if (motion_native == nullptr || depth_native == nullptr) {
        if (submission_silent_budget.admit(3U)) {
            logger::warn(
                "MFG submission stopped: the shared D3D12 motion vector or "
                "depth resource is null (motion={}, depth={}). This path "
                "returned silently before 2026-08-21. A raised deterministic "
                "gate will NOT release while this keeps failing",
                motion_native != nullptr,
                depth_native != nullptr);
        }
        return false;
    }
    const auto motion_description = motion_native->GetDesc();
    const auto depth_description = depth_native->GetDesc();
    const sl::Extent motion_extent{
        0,
        0,
        static_cast<std::uint32_t>(motion_description.Width),
        motion_description.Height};
    const sl::Extent depth_extent{
        0,
        0,
        static_cast<std::uint32_t>(depth_description.Width),
        depth_description.Height};
    if (motion_extent.width == 0 || motion_extent.height == 0 ||
        depth_extent.width == 0 || depth_extent.height == 0) {
        if (!zero_extent_logged_) {
            zero_extent_logged_ = true;
            logger::warn(
                "MFG submission rejected: the submitted motion/depth "
                "resource has a zero extent (motion {}x{}, depth {}x{})",
                motion_extent.width,
                motion_extent.height,
                depth_extent.width,
                depth_extent.height);
        }
        return false;
    }
    {
        const auto& settings = config::Settings::instance();
        tag_contract_.set("multiplier", settings.frame_generation_multiplier());
        tag_contract_.extent(
            "render", render_extent.left, render_extent.top,
            render_extent.width, render_extent.height);
        tag_contract_.extent(
            "output", output_extent.left, output_extent.top,
            output_extent.width, output_extent.height);
        tag_contract_.extent(
            "temporal", temporal_extent.left, temporal_extent.top,
            temporal_extent.width, temporal_extent.height);
        tag_contract_.extent(
            "motion", motion_extent.left, motion_extent.top,
            motion_extent.width, motion_extent.height);
        tag_contract_.extent(
            "depth", depth_extent.left, depth_extent.top,
            depth_extent.width, depth_extent.height);
        tag_contract_.set(
            "upscaling", super_resolution.enabled());
        tag_contract_.set(
            "upscaling-render",
            std::to_string(super_resolution.render_width()) + "x" +
                std::to_string(super_resolution.render_height()));
        tag_contract_.set("tag-hudless-ui", tag_policy.use_hudless_ui);
        tag_contract_.set(
            "tag-backbuffer-extent", tag_policy.tag_backbuffer_extent);
        tag_contract_.set(
            "hudless", resources.frame_generation_hudless() != nullptr);
        tag_contract_.set(
            "ui-color-alpha", resources.ui_color_alpha() != nullptr);
        tag_contract_.publish();
    }

    auto motion = make_resource(motion_native);
    auto depth = make_resource(depth_native);
    auto ui = make_resource(resources.ui_color_alpha());
    std::array<sl::ResourceTag, 5> tags{};
    tags[0] = tag_policy.use_hudless_ui ?
        sl::ResourceTag{
            &color,
            sl::kBufferTypeHUDLessColor,
            sl::ResourceLifecycle::eValidUntilPresent,
            &output_extent} :
        sl::ResourceTag{
            nullptr,
            sl::kBufferTypeHUDLessColor,
            sl::ResourceLifecycle{},
            nullptr};
    tags[1] = {
        &motion,
        sl::kBufferTypeMotionVectors,
        sl::ResourceLifecycle::eValidUntilPresent,
        &motion_extent};
    tags[2] = {
        &depth,
        sl::kBufferTypeDepth,
        sl::ResourceLifecycle::eValidUntilPresent,
        &depth_extent};
    tags[3] = tag_policy.use_hudless_ui ?
        sl::ResourceTag{
            &ui,
            sl::kBufferTypeUIColorAndAlpha,
            sl::ResourceLifecycle::eValidUntilPresent,
            &output_extent} :
        sl::ResourceTag{
            nullptr,
            sl::kBufferTypeUIColorAndAlpha,
            sl::ResourceLifecycle{},
            nullptr};

    tags[4] = tag_policy.tag_backbuffer_extent ?
        sl::ResourceTag{
            nullptr,
            sl::kBufferTypeBackbuffer,
            sl::ResourceLifecycle{},
            &output_extent} :
        sl::ResourceTag{};

    const auto tag_result = api.set_tag_for_frame(
        *frame_token_,
        kMainViewport,
        tags.data(),
        tag_policy.tag_count,
        nullptr);
    if (tag_result != sl::Result::eOk) {
        if (!failure_logged_) {
            logger::error(
                "slSetTagForFrame failed: {}",
                static_cast<int>(tag_result));
            failure_logged_ = true;
        }
        return false;
    }

    failure_logged_ = false;
    if (!first_submission_logged_) {
        first_submission_logged_ = true;
        logger::info(
            "First Streamline frame submitted: token={}, camera constants "
            "and color/motion/depth tags accepted, color-source={}, "
            "color={}x{}, "
            "motion={}x{}, depth={}x{}, output={}x{}, backbuffer={}x{}, "
            "motion-contract={}, "
            "UI tag={}",
            frame_index_,
            tag_policy.use_hudless_ui ?
                "HUD-less+UI" : "automatic-final-color",
            output_extent.width,
            output_extent.height,
            motion_extent.width,
            motion_extent.height,
            depth_extent.width,
            depth_extent.height,
            output_extent.width,
            output_extent.height,
            output_extent.width,
            output_extent.height,
            "raw-current-frame/non-dilated",
            tag_policy.use_hudless_ui);
    }
    return true;
}

bool FrameSubmission::submit_inactive_tags()
{

    reset_pending_ = true;

    if (!timing_ready_ || !frame_open_ || output_width_ == 0 ||
        output_height_ == 0) {
        return false;
    }
    if (!frame_prepared_ && !acquire_frame_token()) {
        return false;
    }

    const sl::Extent output_extent{
        0,
        0,
        output_width_,
        output_height_};
    std::array<sl::ResourceTag, 5> tags{
        sl::ResourceTag{
            nullptr,
            sl::kBufferTypeHUDLessColor,
            sl::ResourceLifecycle{},
            nullptr},
        sl::ResourceTag{
            nullptr,
            sl::kBufferTypeMotionVectors,
            sl::ResourceLifecycle{},
            nullptr},
        sl::ResourceTag{
            nullptr,
            sl::kBufferTypeDepth,
            sl::ResourceLifecycle{},
            nullptr},
        sl::ResourceTag{
            nullptr,
            sl::kBufferTypeUIColorAndAlpha,
            sl::ResourceLifecycle{},
            nullptr},

        sl::ResourceTag{
            nullptr,
            sl::kBufferTypeBackbuffer,
            sl::ResourceLifecycle{},
            &output_extent}};

    const auto result = Api::instance().set_tag_for_frame(
        *frame_token_,
        kMainViewport,
        tags.data(),
        static_cast<std::uint32_t>(tags.size()),
        nullptr);
    if (result != sl::Result::eOk) {
        if (!inactive_tag_failure_logged_) {
            inactive_tag_failure_logged_ = true;
            logger::error(
                "Unable to clear Streamline inputs for an inactive frame: {}",
                static_cast<int>(result));
        }
        return false;
    }

    inactive_tag_failure_logged_ = false;
    if (!inactive_tags_logged_) {
        inactive_tags_logged_ = true;
        logger::info(
            "Inactive Streamline frames now publish null color/motion/depth/UI "
            "tags with an explicit {}x{} backbuffer extent",
            output_width_,
            output_height_);
    }
    return true;
}

void FrameSubmission::request_history_reset() noexcept
{
    reset_pending_ = true;
}

void FrameSubmission::close_simulation_marker()
{
    if (!simulation_open_) {
        return;
    }
    simulation_open_ = false;
    set_marker(static_cast<std::uint32_t>(sl::PCLMarker::eSimulationEnd));
}

void FrameSubmission::render_submit_start()
{
    close_simulation_marker();
    set_marker(
        static_cast<std::uint32_t>(
            sl::PCLMarker::eRenderSubmitStart));
}

void FrameSubmission::render_submit_end()
{
    set_marker(
        static_cast<std::uint32_t>(
            sl::PCLMarker::eRenderSubmitEnd));
}

void FrameSubmission::present_start()
{
    close_simulation_marker();
    set_marker(static_cast<std::uint32_t>(sl::PCLMarker::ePresentStart));
}

void FrameSubmission::present_end()
{
    set_marker(static_cast<std::uint32_t>(sl::PCLMarker::ePresentEnd));
    ++present_count_;
    abandon_frame();
    ++presentation_index_;

    if (present_count_ == 600ULL || present_count_ % 60000ULL == 0ULL) {
        const auto presents_since = present_count_ - reported_presents_;
        const auto opened_since = opened_frame_count_ - reported_opened_frames_;
        const auto duplicates_since =
            duplicate_begin_count_ - reported_duplicate_begins_;
        const auto report =
            present_count_ == 600ULL ||
            presents_since != opened_since ||
            duplicates_since != 0ULL;
        reported_presents_ = present_count_;
        reported_opened_frames_ = opened_frame_count_;
        reported_duplicate_begins_ = duplicate_begin_count_;
        if (report) {
            logger::info(
                "Streamline lifecycle diagnostic: begin-calls={}, "
                "opened-frames={}, presents={}, repeated-begins={}. Since the "
                "last report: {} presents, {} opened frames, {} repeated "
                "begins",
                begin_call_count_,
                opened_frame_count_,
                present_count_,
                duplicate_begin_count_,
                presents_since,
                opened_since,
                duplicates_since);
        }
    }
}

void FrameSubmission::abandon_frame() noexcept
{
    simulation_open_ = false;
    frame_token_ = nullptr;
    frame_open_ = false;
    frame_prepared_ = false;
    constants_submitted_ = false;
}

void FrameSubmission::shutdown() noexcept
{
    abandon_frame();
    reflex_sleep_ = nullptr;
    pcl_set_marker_ = nullptr;
    frame_index_ = 0;
    presentation_index_ = 0;
    output_width_ = 0;
    output_height_ = 0;
    begin_call_count_ = 0;
    opened_frame_count_ = 0;
    present_count_ = 0;
    duplicate_begin_count_ = 0;
    last_camera_sequence_ = 0;
    timing_ready_ = false;
    pcl_get_state_ = nullptr;
    pcl_set_options_ = nullptr;
    pcl_ping_message_ = 0U;
    reflex_sleep_enabled_ = true;
    reset_pending_ = true;
    duplicate_begin_logged_ = false;
    stale_camera_logged_ = false;
    lifecycle_logged_ = false;
    first_submission_logged_ = false;
    inactive_tags_logged_ = false;
    inactive_tag_failure_logged_ = false;
    failure_logged_ = false;
    reduced_viewport_rejection_logged_ = false;
    reduced_ui_rejection_logged_ = false;
}

const sl::FrameToken* FrameSubmission::frame_token() const noexcept
{
    return frame_token_;
}

std::uint32_t FrameSubmission::frame_index() const noexcept
{
    return frame_index_;
}
}
