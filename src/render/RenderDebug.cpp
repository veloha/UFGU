#include "render/RenderDebug.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

constexpr int kCaptureKey = VK_F11;
constexpr int kModeKey = VK_F10;
constexpr int kSceneBlitProbeKey = VK_RIGHT;

constexpr std::array<float, 4> kProbeSentinel{1.0F, 0.0F, 1.0F, 1.0F};

[[nodiscard]] bool key_edge(const int key, bool& was_down) noexcept
{
    const auto control =
        (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const auto down =
        control && (GetAsyncKeyState(key) & 0x8000) != 0;
    const auto edge = down && !was_down;
    was_down = down;
    return edge;
}

[[nodiscard]] std::string_view mode_name(
    const UiCompositeMode mode) noexcept
{
    switch (mode) {
    case UiCompositeMode::signed_delta:
        return "signed-delta (upscaled + (with_ui - before_ui))";
    case UiCompositeMode::coverage:
        return "coverage (with_ui + (1 - coverage) * base-difference)";
    case UiCompositeMode::exact:
        return "exact (reduced composited colour on UI pixels)";
    }
    return "signed-delta";
}

struct ChannelLayout
{
    std::array<std::size_t, 4> source{};
    bool supported{};
};

[[nodiscard]] ChannelLayout channel_layout(
    const DXGI_FORMAT format) noexcept
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:

        return ChannelLayout{
            std::array<std::size_t, 4>{2, 1, 0, 3},
            true};
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return ChannelLayout{
            std::array<std::size_t, 4>{0, 1, 2, 3},
            true};
    default:
        return ChannelLayout{
            std::array<std::size_t, 4>{0, 1, 2, 3},
            false};
    }
}

void append_u16(std::vector<char>& bytes, const std::uint16_t value)
{
    bytes.push_back(static_cast<char>(value & 0xFFU));
    bytes.push_back(static_cast<char>((value >> 8) & 0xFFU));
}

void append_u32(std::vector<char>& bytes, const std::uint32_t value)
{
    bytes.push_back(static_cast<char>(value & 0xFFU));
    bytes.push_back(static_cast<char>((value >> 8) & 0xFFU));
    bytes.push_back(static_cast<char>((value >> 16) & 0xFFU));
    bytes.push_back(static_cast<char>((value >> 24) & 0xFFU));
}

[[nodiscard]] std::optional<std::filesystem::path> capture_directory(
    bool& failure_logged)
{
    auto directory = logger::log_directory();
    if (!directory) {
        if (!failure_logged) {
            failure_logged = true;
            logger::error(
                "Render debug capture is unavailable: the SKSE log "
                "directory could not be resolved");
        }
        return std::nullopt;
    }
    *directory /= "UFGU-captures";
    std::error_code error;
    std::filesystem::create_directories(*directory, error);
    if (error) {
        if (!failure_logged) {
            failure_logged = true;
            logger::error(
                "Render debug capture directory {} could not be created: {}",
                directory->string(),
                error.message());
        }
        return std::nullopt;
    }
    failure_logged = false;
    return directory;
}

[[nodiscard]] std::string timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm parts{};
    if (localtime_s(&parts, &time) != 0) {
        return "00000000-000000";
    }
    std::array<char, 32> buffer{};
    const auto written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04d%02d%02d-%02d%02d%02d",
        parts.tm_year + 1900,
        parts.tm_mon + 1,
        parts.tm_mday,
        parts.tm_hour,
        parts.tm_min,
        parts.tm_sec);
    if (written <= 0) {
        return "00000000-000000";
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}
}

RenderDebug& RenderDebug::instance() noexcept
{
    static RenderDebug debug;
    return debug;
}

void RenderDebug::poll_hotkeys()
{
    if (key_edge(kCaptureKey, capture_key_was_down_) &&
        !capture_pending_) {
        capture_pending_ = true;
        ++capture_index_;
        files_written_ = 0;
        logger::info(
            "Render debug capture {} requested; the next composited frame "
            "will be written to disk",
            capture_index_);
    }
    if (key_edge(kModeKey, mode_key_was_down_)) {
        composite_mode_ =
            composite_mode_ == UiCompositeMode::signed_delta ?
                UiCompositeMode::coverage :
                (composite_mode_ == UiCompositeMode::coverage ?
                     UiCompositeMode::exact :
                     UiCompositeMode::signed_delta);
        logger::info(
            "UI composite mode changed to {}",
            mode_name(composite_mode_));
    }

}

bool RenderDebug::scene_blit_probe_armed() const noexcept
{
    return probe_armed_;
}

UiCompositeMode RenderDebug::composite_mode() const noexcept
{
    return composite_mode_;
}

bool RenderDebug::capture_pending() const noexcept
{
    return capture_pending_;
}

void RenderDebug::dump(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    const std::string_view label)
{
    if (!capture_pending_ || device == nullptr || context == nullptr ||
        texture == nullptr) {
        return;
    }

    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Width == 0 || description.Height == 0 ||
        description.SampleDesc.Count != 1) {
        logger::warn(
            "Render debug capture skipped {}: {}x{}, samples={}",
            label,
            description.Width,
            description.Height,
            description.SampleDesc.Count);
        return;
    }

    const auto layout = channel_layout(description.Format);
    if (!layout.supported) {
        logger::warn(
            "Render debug capture skipped {}: unsupported format {}",
            label,
            static_cast<unsigned>(description.Format));
        return;
    }

    auto staging_description = description;
    staging_description.MipLevels = 1;
    staging_description.ArraySize = 1;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.BindFlags = 0;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_description.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    auto result = device->CreateTexture2D(
        &staging_description,
        nullptr,
        &staging);
    if (FAILED(result)) {
        logger::error(
            "Render debug capture could not allocate a staging surface for "
            "{}: 0x{:08X}",
            label,
            static_cast<unsigned>(result));
        return;
    }

    context->CopySubresourceRegion(
        staging.Get(),
        0,
        0,
        0,
        0,
        texture,
        0,
        nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(
        staging.Get(),
        0,
        D3D11_MAP_READ,
        0,
        &mapped);
    if (FAILED(result)) {
        logger::error(
            "Render debug capture could not map {}: 0x{:08X}",
            label,
            static_cast<unsigned>(result));
        return;
    }

    const auto width = static_cast<std::size_t>(description.Width);
    const auto height = static_cast<std::size_t>(description.Height);
    const auto row_bytes = width * 4U;

    std::vector<char> bytes;
    const auto pixel_bytes = row_bytes * height;
    bytes.reserve(pixel_bytes + 54U);
    bytes.push_back('B');
    bytes.push_back('M');
    append_u32(bytes, static_cast<std::uint32_t>(pixel_bytes + 54U));
    append_u16(bytes, std::uint16_t{0});
    append_u16(bytes, std::uint16_t{0});
    append_u32(bytes, std::uint32_t{54});
    append_u32(bytes, std::uint32_t{40});
    append_u32(bytes, static_cast<std::uint32_t>(description.Width));
    append_u32(bytes, static_cast<std::uint32_t>(description.Height));
    append_u16(bytes, std::uint16_t{1});
    append_u16(bytes, std::uint16_t{32});
    append_u32(bytes, std::uint32_t{0});
    append_u32(bytes, static_cast<std::uint32_t>(pixel_bytes));
    append_u32(bytes, std::uint32_t{0});
    append_u32(bytes, std::uint32_t{0});
    append_u32(bytes, std::uint32_t{0});
    append_u32(bytes, std::uint32_t{0});

    std::array<std::uint64_t, 4> channel_totals{};
    std::array<std::uint8_t, 4> channel_minimums{255, 255, 255, 255};
    std::array<std::uint8_t, 4> channel_maximums{};
    std::uint64_t nonzero_alpha{};

    for (std::size_t row = 0; row < height; ++row) {
        const auto* source =
            static_cast<const std::uint8_t*>(mapped.pData) +
            ((height - 1U - row) * static_cast<std::size_t>(mapped.RowPitch));
        for (std::size_t column = 0; column < width; ++column) {
            const auto* pixel = source + (column * 4U);
            for (std::size_t channel = 0; channel < 4U; ++channel) {
                const auto value = pixel[layout.source[channel]];
                bytes.push_back(static_cast<char>(value));
            }
            for (std::size_t channel = 0; channel < 4U; ++channel) {
                const auto value = pixel[channel];
                channel_totals[channel] += value;
                channel_minimums[channel] =
                    (std::min)(channel_minimums[channel], value);
                channel_maximums[channel] =
                    (std::max)(channel_maximums[channel], value);
            }
            nonzero_alpha += pixel[3] != 0 ? 1U : 0U;
        }
    }
    context->Unmap(staging.Get(), 0);

    const auto directory = capture_directory(directory_failure_logged_);
    if (!directory) {
        return;
    }

    const auto name =
        "UFGU-" + timestamp() + "-" +
        std::to_string(capture_index_) + "-" + std::string{label} +
        "-" + std::to_string(description.Width) + "x" +
        std::to_string(description.Height) + ".bmp";
    const auto file = *directory / name;
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        logger::error(
            "Render debug capture could not open {}",
            file.string());
        return;
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        logger::error(
            "Render debug capture could not write {}",
            file.string());
        return;
    }
    stream.close();
    ++files_written_;

    const auto pixels = static_cast<double>(width * height);
    logger::info(
        "Render debug capture {} wrote {} ({}x{}, format={}): "
        "mean R/G/B/A={:.4f}/{:.4f}/{:.4f}/{:.4f}, "
        "min R/G/B/A={}/{}/{}/{}, max R/G/B/A={}/{}/{}/{}, "
        "nonzero-alpha={:.3f}%",
        capture_index_,
        name,
        description.Width,
        description.Height,
        static_cast<unsigned>(description.Format),
        static_cast<double>(channel_totals[0]) / (pixels * 255.0),
        static_cast<double>(channel_totals[1]) / (pixels * 255.0),
        static_cast<double>(channel_totals[2]) / (pixels * 255.0),
        static_cast<double>(channel_totals[3]) / (pixels * 255.0),
        channel_minimums[0],
        channel_minimums[1],
        channel_minimums[2],
        channel_minimums[3],
        channel_maximums[0],
        channel_maximums[1],
        channel_maximums[2],
        channel_maximums[3],
        (static_cast<double>(nonzero_alpha) * 100.0) / pixels);
}

void RenderDebug::end_capture()
{
    if (!capture_pending_) {
        return;
    }
    capture_pending_ = false;
    const auto directory = capture_directory(directory_failure_logged_);
    logger::info(
        "Render debug capture {} complete: {} files in {} "
        "(active UI composite mode: {})",
        capture_index_,
        files_written_,
        directory ? directory->string() : std::string{"<unavailable>"},
        mode_name(composite_mode_));
}

void RenderDebug::log_present_boundary_state(
    ID3D11DeviceContext* const context,
    void* const window,
    const unsigned logical_width,
    const unsigned logical_height,
    const unsigned physical_width,
    const unsigned physical_height)
{

    constexpr std::uint32_t kMaximumReports{24};
    if (context == nullptr || boundary_reports_ >= kMaximumReports) {
        return;
    }

    RECT client{};
    const auto client_known =
        window != nullptr &&
        GetClientRect(static_cast<HWND>(window), &client) != 0;
    const auto client_width =
        client_known ? static_cast<unsigned>(client.right - client.left) : 0U;
    const auto client_height =
        client_known ? static_cast<unsigned>(client.bottom - client.top) : 0U;

    constexpr UINT kStateSlots{
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE};
    std::array<D3D11_VIEWPORT, kStateSlots> viewports{};
    auto viewport_count = kStateSlots;
    context->RSGetViewports(&viewport_count, viewports.data());

    std::array<D3D11_RECT, kStateSlots> scissors{};
    auto scissor_count = kStateSlots;
    context->RSGetScissorRects(&scissor_count, scissors.data());

    ComPtr<ID3D11RenderTargetView> render_target;
    context->OMGetRenderTargets(1, render_target.GetAddressOf(), nullptr);
    unsigned target_width{};
    unsigned target_height{};
    unsigned target_format{};
    if (render_target) {
        ComPtr<ID3D11Resource> resource;
        render_target->GetResource(resource.GetAddressOf());
        ComPtr<ID3D11Texture2D> texture;
        if (resource && SUCCEEDED(resource.As(&texture))) {
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            target_width = description.Width;
            target_height = description.Height;
            target_format = static_cast<unsigned>(description.Format);
        }
    }

    const auto& viewport = viewports[0];
    const auto& scissor = scissors[0];
    auto signature =
        std::to_string(client_width) + "x" + std::to_string(client_height) +
        "|" + std::to_string(target_width) + "x" +
        std::to_string(target_height) + "|" +
        std::to_string(viewport_count) + ":" +
        std::to_string(static_cast<int>(viewport.Width)) + "x" +
        std::to_string(static_cast<int>(viewport.Height)) + "|" +
        std::to_string(scissor_count) + ":" +
        std::to_string(scissor.left) + "," + std::to_string(scissor.top) +
        "," + std::to_string(scissor.right) + "," +
        std::to_string(scissor.bottom);
    if (signature == boundary_signature_) {
        return;
    }
    boundary_signature_ = std::move(signature);
    ++boundary_reports_;

    const auto scissor_exceeds_target =
        target_width != 0U && target_height != 0U &&
        (scissor.right > static_cast<LONG>(target_width) ||
         scissor.bottom > static_cast<LONG>(target_height));

    logger::info(
        "Present-boundary state {}: client={}x{}, logical-render={}x{}, "
        "physical-output={}x{}, bound-target={}x{} (format={}), "
        "viewports={} first={}x{} at {},{}, scissors={} first={},{},{},{}, "
        "scissor-exceeds-target={}",
        boundary_reports_,
        client_width,
        client_height,
        logical_width,
        logical_height,
        physical_width,
        physical_height,
        target_width,
        target_height,
        target_format,
        viewport_count,
        static_cast<int>(viewport.Width),
        static_cast<int>(viewport.Height),
        static_cast<int>(viewport.TopLeftX),
        static_cast<int>(viewport.TopLeftY),
        scissor_count,
        scissor.left,
        scissor.top,
        scissor.right,
        scissor.bottom,
        scissor_exceeds_target);
}

struct SceneBlitProbeState
{
    ComPtr<ID3D11Texture2D> scratch;
    ComPtr<ID3D11RenderTargetView> scratch_view;
    ComPtr<ID3D11ShaderResourceView> scratch_resource_view;
    ComPtr<ID3D11Texture2D> staging;

    ID3D11Texture2D* saved_texture{};
    ID3D11RenderTargetView* saved_view{};
    ID3D11ShaderResourceView* saved_resource_view{};

    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        saved_viewports{};
    UINT saved_viewport_count{};
    std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        saved_scissors{};
    UINT saved_scissor_count{};

    unsigned physical_width{};
    unsigned physical_height{};
    unsigned render_width{};
    unsigned render_height{};
    bool active{};
};

RenderDebug::~RenderDebug() = default;

bool RenderDebug::begin_scene_blit_probe(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const description_source,
    const unsigned physical_width,
    const unsigned physical_height,
    const unsigned render_width,
    const unsigned render_height)
{
    if (!probe_armed_ || device == nullptr || context == nullptr ||
        physical_width == 0U || physical_height == 0U) {
        return false;
    }

    probe_armed_ = false;

    auto* renderer = RE::BSGraphics::Renderer::GetRendererData();
    if (renderer == nullptr) {
        logger::error("Scene-blit probe: no renderer data");
        return false;
    }
    auto& framebuffer =
        renderer->renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
    auto* source =
        framebuffer.texture != nullptr ? framebuffer.texture :
                                         description_source;
    if (source == nullptr) {
        logger::error(
            "Scene-blit probe: no framebuffer resource description available");
        return false;
    }

    D3D11_TEXTURE2D_DESC source_description{};
    source->GetDesc(&source_description);

    auto probe = std::make_unique<SceneBlitProbeState>();

    auto description = source_description;
    description.Width = physical_width;
    description.Height = physical_height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags = 0;
    description.MiscFlags = 0;
    auto result = device->CreateTexture2D(
        &description,
        nullptr,
        probe->scratch.GetAddressOf());
    if (FAILED(result)) {
        logger::error(
            "Scene-blit probe: scratch surface creation failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }
    result = device->CreateRenderTargetView(
        probe->scratch.Get(),
        nullptr,
        probe->scratch_view.GetAddressOf());
    if (FAILED(result)) {
        logger::error(
            "Scene-blit probe: scratch view creation failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }

    static_cast<void>(device->CreateShaderResourceView(
        probe->scratch.Get(),
        nullptr,
        probe->scratch_resource_view.GetAddressOf()));

    auto staging_description = description;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.BindFlags = 0;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    result = device->CreateTexture2D(
        &staging_description,
        nullptr,
        probe->staging.GetAddressOf());
    if (FAILED(result)) {
        logger::error(
            "Scene-blit probe: staging surface creation failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }

    probe->physical_width = physical_width;
    probe->physical_height = physical_height;
    probe->render_width = render_width;
    probe->render_height = render_height;

    probe->saved_viewport_count =
        static_cast<UINT>(probe->saved_viewports.size());
    context->RSGetViewports(
        &probe->saved_viewport_count,
        probe->saved_viewports.data());
    probe->saved_scissor_count =
        static_cast<UINT>(probe->saved_scissors.size());
    context->RSGetScissorRects(
        &probe->saved_scissor_count,
        probe->saved_scissors.data());

    probe->saved_texture = framebuffer.texture;
    probe->saved_view = framebuffer.RTV;
    probe->saved_resource_view = framebuffer.SRV;
    framebuffer.texture = probe->scratch.Get();
    framebuffer.RTV = probe->scratch_view.Get();
    if (probe->scratch_resource_view) {
        framebuffer.SRV = probe->scratch_resource_view.Get();
    }

    context->ClearRenderTargetView(
        probe->scratch_view.Get(),
        kProbeSentinel.data());

    probe->active = true;
    probe_ = std::move(probe);
    logger::info(
        "Scene-blit probe: substituted a {}x{} scratch framebuffer (source "
        "{}x{}, format={}) while the render extent stays {}x{}",
        physical_width,
        physical_height,
        source_description.Width,
        source_description.Height,
        static_cast<unsigned>(source_description.Format),
        render_width,
        render_height);
    return true;
}

void RenderDebug::end_scene_blit_probe(ID3D11DeviceContext* const context)
{
    if (probe_ == nullptr || !probe_->active || context == nullptr) {
        return;
    }
    probe_->active = false;

    constexpr UINT kStateSlots{
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE};
    std::array<D3D11_VIEWPORT, kStateSlots> viewports{};
    auto viewport_count = kStateSlots;
    context->RSGetViewports(&viewport_count, viewports.data());
    std::array<D3D11_RECT, kStateSlots> scissors{};
    auto scissor_count = kStateSlots;
    context->RSGetScissorRects(&scissor_count, scissors.data());

    unsigned bound_width{};
    unsigned bound_height{};
    {
        ComPtr<ID3D11RenderTargetView> bound;
        context->OMGetRenderTargets(1, bound.GetAddressOf(), nullptr);
        if (bound) {
            ComPtr<ID3D11Resource> resource;
            bound->GetResource(resource.GetAddressOf());
            ComPtr<ID3D11Texture2D> texture;
            if (resource && SUCCEEDED(resource.As(&texture))) {
                D3D11_TEXTURE2D_DESC bound_description{};
                texture->GetDesc(&bound_description);
                bound_width = bound_description.Width;
                bound_height = bound_description.Height;
            }
        }
    }

    if (auto* renderer = RE::BSGraphics::Renderer::GetRendererData();
        renderer != nullptr) {
        auto& framebuffer =
            renderer->renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
        framebuffer.texture = probe_->saved_texture;
        framebuffer.RTV = probe_->saved_view;
        framebuffer.SRV = probe_->saved_resource_view;
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);
    if (probe_->saved_viewport_count != 0U) {
        context->RSSetViewports(
            probe_->saved_viewport_count,
            probe_->saved_viewports.data());
    }
    if (probe_->saved_scissor_count != 0U) {
        context->RSSetScissorRects(
            probe_->saved_scissor_count,
            probe_->saved_scissors.data());
    }

    context->CopyResource(probe_->staging.Get(), probe_->scratch.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto result =
        context->Map(probe_->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        logger::error(
            "Scene-blit probe: staging map failed: 0x{:08X}",
            static_cast<unsigned>(result));
        probe_.reset();
        return;
    }

    D3D11_TEXTURE2D_DESC written{};
    probe_->staging->GetDesc(&written);
    const auto layout = channel_layout(written.Format);

    std::array<std::uint8_t, 4> sentinel{};
    for (std::size_t channel = 0; channel < 4U; ++channel) {
        sentinel[channel] =
            static_cast<std::uint8_t>(kProbeSentinel[channel] * 255.0F + 0.5F);
    }
    if (layout.source[0] == 0U && layout.supported) {

        std::swap(sentinel[0], sentinel[2]);
    }

    const auto width = static_cast<std::size_t>(written.Width);
    const auto height = static_cast<std::size_t>(written.Height);
    std::uint64_t inside{};
    std::uint64_t outside{};
    auto min_x = width;
    auto min_y = height;
    std::size_t max_x{};
    std::size_t max_y{};
    for (std::size_t row = 0; row < height; ++row) {
        const auto* source =
            static_cast<const std::uint8_t*>(mapped.pData) +
            (row * static_cast<std::size_t>(mapped.RowPitch));
        for (std::size_t column = 0; column < width; ++column) {
            const auto* pixel = source + (column * 4U);
            if (pixel[0] == sentinel[0] && pixel[1] == sentinel[1] &&
                pixel[2] == sentinel[2]) {
                continue;
            }
            if (column < probe_->render_width &&
                row < probe_->render_height) {
                ++inside;
            } else {
                ++outside;
            }
            min_x = (std::min)(min_x, column);
            min_y = (std::min)(min_y, row);
            max_x = (std::max)(max_x, column);
            max_y = (std::max)(max_y, row);
        }
    }
    context->Unmap(probe_->staging.Get(), 0);

    const auto inside_area =
        static_cast<double>(probe_->render_width) *
        static_cast<double>(probe_->render_height);
    const auto outside_area =
        (static_cast<double>(width) * static_cast<double>(height)) -
        inside_area;
    const auto inside_coverage =
        inside_area > 0.0 ?
            (static_cast<double>(inside) * 100.0) / inside_area :
            0.0;
    const auto outside_coverage =
        outside_area > 0.0 ?
            (static_cast<double>(outside) * 100.0) / outside_area :
            0.0;

    std::string_view verdict{"inconclusive"};
    if (inside + outside == 0U) {
        verdict = "inconclusive-no-write";
    } else if (outside_coverage < 1.0 && inside_coverage > 50.0) {
        verdict = "viewport-respected";
    } else if (outside_coverage > 50.0) {
        verdict = "scene-stretched";
    }

    logger::info(
        "Scene-blit probe result: destination={}x{} (format={}), "
        "bound-target={}x{}, viewports={} first={}x{} at {},{}, "
        "scissors={} first={},{},{},{}, render-extent={}x{}, "
        "written-bounds={},{}..{},{}, inside-coverage={:.2f}%, "
        "outside-coverage={:.2f}%, verdict={}",
        written.Width,
        written.Height,
        static_cast<unsigned>(written.Format),
        bound_width,
        bound_height,
        viewport_count,
        static_cast<int>(viewports[0].Width),
        static_cast<int>(viewports[0].Height),
        static_cast<int>(viewports[0].TopLeftX),
        static_cast<int>(viewports[0].TopLeftY),
        scissor_count,
        scissors[0].left,
        scissors[0].top,
        scissors[0].right,
        scissors[0].bottom,
        probe_->render_width,
        probe_->render_height,
        min_x > max_x ? 0U : static_cast<unsigned>(min_x),
        min_y > max_y ? 0U : static_cast<unsigned>(min_y),
        static_cast<unsigned>(max_x),
        static_cast<unsigned>(max_y),
        inside_coverage,
        outside_coverage,
        verdict);

    probe_.reset();
}

namespace
{

struct ObservedTarget
{
    const void* requested{};
    const void* applied{};
    unsigned requested_width{};
    unsigned requested_height{};
    unsigned requested_format{};
    unsigned applied_width{};
    unsigned applied_height{};
    unsigned applied_format{};
    float viewport_width{};
    float viewport_height{};
    LONG scissor_width{};
    LONG scissor_height{};
    bool after_main_draw{};
    bool redirected{};
    bool unordered_access_variant{};
};

struct TraceSurface
{
    ComPtr<ID3D11Texture2D> copy;
    ComPtr<ID3D11Texture2D> staging;
    unsigned width{};
    unsigned height{};
    unsigned format{};
};

struct DifferenceResult
{
    bool valid{};
    std::uint64_t changed{};
    std::uint64_t total{};
    unsigned min_x{};
    unsigned min_y{};
    unsigned max_x{};
    unsigned max_y{};
};

[[nodiscard]] bool capture_surface(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const source,
    TraceSurface& surface,
    const char* const label)
{
    if (device == nullptr || context == nullptr || source == nullptr) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    if (surface.copy == nullptr ||
        surface.width != description.Width ||
        surface.height != description.Height ||
        surface.format != static_cast<unsigned>(description.Format)) {
        surface.copy.Reset();
        surface.staging.Reset();
        auto copy_description = description;
        copy_description.Usage = D3D11_USAGE_DEFAULT;
        copy_description.BindFlags = 0;
        copy_description.CPUAccessFlags = 0;
        copy_description.MiscFlags = 0;
        copy_description.MipLevels = 1;
        copy_description.ArraySize = 1;
        auto result = device->CreateTexture2D(
            &copy_description,
            nullptr,
            surface.copy.GetAddressOf());
        if (FAILED(result)) {
            logger::error(
                "Scaleform trace: could not allocate the {} copy: 0x{:08X}",
                label,
                static_cast<unsigned>(result));
            return false;
        }
        auto staging_description = copy_description;
        staging_description.Usage = D3D11_USAGE_STAGING;
        staging_description.BindFlags = 0;
        staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        result = device->CreateTexture2D(
            &staging_description,
            nullptr,
            surface.staging.GetAddressOf());
        if (FAILED(result)) {
            logger::error(
                "Scaleform trace: could not allocate the {} staging "
                "surface: 0x{:08X}",
                label,
                static_cast<unsigned>(result));
            surface.copy.Reset();
            return false;
        }
        surface.width = description.Width;
        surface.height = description.Height;
        surface.format = static_cast<unsigned>(description.Format);
    }
    context->CopyResource(surface.copy.Get(), source);
    return true;
}

[[nodiscard]] DifferenceResult compare_surfaces(
    ID3D11DeviceContext* const context,
    TraceSurface& first,
    TraceSurface& second)
{
    DifferenceResult difference{};
    if (context == nullptr ||
        first.copy == nullptr || second.copy == nullptr ||
        first.width != second.width || first.height != second.height ||
        first.format != second.format ||
        first.width == 0U || first.height == 0U) {
        return difference;
    }
    context->CopyResource(first.staging.Get(), first.copy.Get());
    context->CopyResource(second.staging.Get(), second.copy.Get());

    D3D11_MAPPED_SUBRESOURCE first_map{};
    D3D11_MAPPED_SUBRESOURCE second_map{};
    if (FAILED(context->Map(
            first.staging.Get(), 0, D3D11_MAP_READ, 0, &first_map))) {
        return difference;
    }
    if (FAILED(context->Map(
            second.staging.Get(), 0, D3D11_MAP_READ, 0, &second_map))) {
        context->Unmap(first.staging.Get(), 0);
        return difference;
    }

    const auto width = static_cast<std::size_t>(first.width);
    const auto height = static_cast<std::size_t>(first.height);
    const auto pixel_bytes =
        first.format ==
                static_cast<unsigned>(DXGI_FORMAT_R16G16B16A16_FLOAT) ?
            8U :
            4U;
    const auto color_bytes = pixel_bytes == 8U ? 6U : 3U;
    auto min_x = width;
    auto min_y = height;
    std::size_t max_x{};
    std::size_t max_y{};
    std::uint64_t changed{};
    for (std::size_t row = 0; row < height; ++row) {
        const auto* left =
            static_cast<const std::uint8_t*>(first_map.pData) +
            (row * static_cast<std::size_t>(first_map.RowPitch));
        const auto* right =
            static_cast<const std::uint8_t*>(second_map.pData) +
            (row * static_cast<std::size_t>(second_map.RowPitch));
        for (std::size_t column = 0; column < width; ++column) {
            const auto offset = column * pixel_bytes;
            auto same = true;
            for (std::size_t byte = 0; byte < color_bytes; ++byte) {
                same &= left[offset + byte] == right[offset + byte];
            }
            if (same) {
                continue;
            }
            ++changed;
            min_x = (std::min)(min_x, column);
            min_y = (std::min)(min_y, row);
            max_x = (std::max)(max_x, column);
            max_y = (std::max)(max_y, row);
        }
    }
    context->Unmap(second.staging.Get(), 0);
    context->Unmap(first.staging.Get(), 0);

    difference.valid = true;
    difference.changed = changed;
    difference.total = static_cast<std::uint64_t>(width * height);
    difference.min_x = min_x > max_x ? 0U : static_cast<unsigned>(min_x);
    difference.min_y = min_y > max_y ? 0U : static_cast<unsigned>(min_y);
    difference.max_x = static_cast<unsigned>(max_x);
    difference.max_y = static_cast<unsigned>(max_y);
    return difference;
}

[[nodiscard]] bool skyrim_hud_menu_open()
{

    auto* ui = RE::UI::GetSingleton();
    return ui != nullptr && ui->IsMenuOpen(RE::HUDMenu::MENU_NAME);
}
}

struct ScaleformTraceState
{
    TraceSurface reduced_before;
    TraceSurface reduced_after;
    TraceSurface native_before_ui;
    TraceSurface main_draw_target_before;
    TraceSurface main_draw_target_after;
    TraceSurface target_episode_before;
    TraceSurface target_episode_after;
    ComPtr<ID3D11Texture2D> main_draw_target;
    ComPtr<ID3D11Texture2D> reduced_target;
    ComPtr<ID3D11Texture2D> native_target;
    ComPtr<ID3D11Texture2D> target_episode_target;
    std::array<ObservedTarget, 64> targets{};
    std::size_t target_count{};
    std::uint32_t overflow_binds{};
    std::uint32_t target_episode_bind{};
    bool active{};
    bool after_main_draw{};
    bool before_captured{};
    bool after_captured{};
    bool main_draw_target_before_captured{};
    bool target_episode_active{};
    bool target_episode_before_captured{};
};

void RenderDebug::scaleform_trace_arm_if_ready()
{

    constexpr std::uint32_t kMaximumRuns{2};

    constexpr std::uint32_t kReadyFrames{120};

    if (trace_runs_ >= kMaximumRuns || (trace_ != nullptr && trace_->active)) {
        return;
    }
    if (!skyrim_hud_menu_open()) {
        trace_ready_frames_ = 0;
        return;
    }
    if (trace_ready_frames_ < kReadyFrames) {
        ++trace_ready_frames_;
        return;
    }
    if (trace_runs_ > 0 && !trace_rearm_requested_) {
        return;
    }
    trace_rearm_requested_ = false;

    if (trace_ == nullptr) {
        trace_ = std::make_unique<ScaleformTraceState>();
    }
    trace_->active = true;
    trace_->after_main_draw = false;
    trace_->before_captured = false;
    trace_->after_captured = false;
    trace_->main_draw_target_before_captured = false;
    trace_->target_count = 0;
    trace_->overflow_binds = 0;
    trace_->main_draw_target.Reset();
    trace_->reduced_target.Reset();
    trace_->native_target.Reset();
    trace_->target_episode_target.Reset();
    trace_->target_episode_bind = 0;
    trace_->target_episode_active = false;
    trace_->target_episode_before_captured = false;
    ++trace_runs_;
    logger::info(
        "Scaleform write-target trace {} armed for this frame "
        "(Skyrim HUD menu open)",
        trace_runs_);
}

bool RenderDebug::scaleform_trace_active() const noexcept
{
    return trace_ != nullptr && trace_->active;
}

void RenderDebug::scaleform_trace_observe_target(
    ID3D11DeviceContext* const context,
    ID3D11RenderTargetView* const requested_view,
    ID3D11RenderTargetView* const applied_view,
    const bool unordered_access_variant)
{
    if (trace_ == nullptr || !trace_->active || requested_view == nullptr) {
        return;
    }
    if (trace_->target_count >= trace_->targets.size()) {
        ++trace_->overflow_binds;
        return;
    }

    ObservedTarget entry{};
    entry.after_main_draw = trace_->after_main_draw;
    entry.unordered_access_variant = unordered_access_variant;
    const auto describe =
        [](ID3D11RenderTargetView* const view,
           const void*& identity,
           unsigned& width,
           unsigned& height,
           unsigned& format) {
            if (view == nullptr) {
                return;
            }
            ComPtr<ID3D11Resource> resource;
            view->GetResource(resource.GetAddressOf());
            identity = static_cast<const void*>(resource.Get());
            ComPtr<ID3D11Texture2D> texture;
            if (resource && SUCCEEDED(resource.As(&texture))) {
                D3D11_TEXTURE2D_DESC description{};
                texture->GetDesc(&description);
                width = description.Width;
                height = description.Height;
                format = static_cast<unsigned>(description.Format);
            }
        };
    describe(
        requested_view,
        entry.requested,
        entry.requested_width,
        entry.requested_height,
        entry.requested_format);
    describe(
        applied_view,
        entry.applied,
        entry.applied_width,
        entry.applied_height,
        entry.applied_format);
    entry.redirected = entry.requested != entry.applied;

    if (context != nullptr) {
        D3D11_VIEWPORT viewport{};
        UINT viewport_count = 1;
        context->RSGetViewports(&viewport_count, &viewport);
        if (viewport_count != 0U) {
            entry.viewport_width = viewport.Width;
            entry.viewport_height = viewport.Height;
        }
        D3D11_RECT scissor{};
        UINT scissor_count = 1;
        context->RSGetScissorRects(&scissor_count, &scissor);
        if (scissor_count != 0U) {
            entry.scissor_width = scissor.right - scissor.left;
            entry.scissor_height = scissor.bottom - scissor.top;
        }
    }

    trace_->targets[trace_->target_count] = entry;
    ++trace_->target_count;
}

void RenderDebug::scaleform_trace_end_target_episode(
    ID3D11DeviceContext* const context)
{
    if (trace_ == nullptr || !trace_->active ||
        !trace_->target_episode_active || context == nullptr) {
        return;
    }

    trace_->target_episode_active = false;
    ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    auto difference = DifferenceResult{};
    const auto after_captured =
        trace_->target_episode_target != nullptr &&
        capture_surface(
            device.Get(),
            context,
            trace_->target_episode_target.Get(),
            trace_->target_episode_after,
            "native target episode end");
    if (trace_->target_episode_before_captured && after_captured) {
        difference = compare_surfaces(
            context,
            trace_->target_episode_before,
            trace_->target_episode_after);
    }

    if (!difference.valid) {
        logger::warn(
            "Scaleform trace: target episode for bind {} could not be "
            "compared",
            trace_->target_episode_bind);
        return;
    }
    const auto percent =
        difference.total != 0U ?
            (static_cast<double>(difference.changed) * 100.0) /
                static_cast<double>(difference.total) :
            0.0;
    logger::info(
        "Scaleform trace: target episode for bind {} changed-pixels={} of {} "
        "({:.4f}%), changed-bounds={},{}..{},{}, verdict={}",
        trace_->target_episode_bind,
        difference.changed,
        difference.total,
        percent,
        difference.min_x,
        difference.min_y,
        difference.max_x,
        difference.max_y,
        difference.changed == 0U ?
            "target-episode-no-change" :
            "target-episode-changed");
}

void RenderDebug::scaleform_trace_begin_target_episode(
    ID3D11DeviceContext* const context,
    ID3D11RenderTargetView* const applied_view)
{
    if (trace_ == nullptr || !trace_->active ||
        !trace_->after_main_draw || context == nullptr ||
        applied_view == nullptr) {
        return;
    }

    ComPtr<ID3D11Resource> resource;
    applied_view->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture)) ||
        (texture.Get() != trace_->reduced_target.Get() &&
         texture.Get() != trace_->native_target.Get())) {
        return;
    }

    ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    trace_->target_episode_target = texture;
    trace_->target_episode_bind =
        static_cast<std::uint32_t>(trace_->target_count);
    trace_->target_episode_before_captured = capture_surface(
        device.Get(),
        context,
        texture.Get(),
        trace_->target_episode_before,
        "native target episode start");
    trace_->target_episode_active =
        trace_->target_episode_before_captured;
}

void RenderDebug::scaleform_trace_before_main_draw(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const reduced,
    ID3D11Texture2D* const native)
{
    if (trace_ == nullptr || !trace_->active) {
        return;
    }
    trace_->reduced_target = reduced;
    trace_->native_target = native;
    trace_->before_captured =
        capture_surface(
            device,
            context,
            reduced,
            trace_->reduced_before,
            "reduced pre-MainDraw");
    static_cast<void>(
        capture_surface(
            device,
            context,
            native,
            trace_->native_before_ui,
            "native post-resolve"));

    unsigned bound_width{};
    unsigned bound_height{};
    unsigned bound_format{};
    const void* bound_resource{};
    if (context != nullptr) {
        ComPtr<ID3D11RenderTargetView> bound;
        context->OMGetRenderTargets(1, bound.GetAddressOf(), nullptr);
        if (bound) {
            ComPtr<ID3D11Resource> resource;
            bound->GetResource(resource.GetAddressOf());
            bound_resource = static_cast<const void*>(resource.Get());
            ComPtr<ID3D11Texture2D> texture;
            if (resource && SUCCEEDED(resource.As(&texture))) {
                D3D11_TEXTURE2D_DESC description{};
                texture->GetDesc(&description);
                bound_width = description.Width;
                bound_height = description.Height;
                bound_format = static_cast<unsigned>(description.Format);
                trace_->main_draw_target = texture;
                trace_->main_draw_target_before_captured = capture_surface(
                    device,
                    context,
                    texture.Get(),
                    trace_->main_draw_target_before,
                    "bound pre-MainDraw");
            }
        }
    }
    logger::info(
        "Scaleform trace: before MainDraw, reduced-proxy={} ({}x{} format={}), "
        "bound-rtv-resource={} ({}x{} format={}), snapshot={}",
        static_cast<const void*>(reduced),
        trace_->reduced_before.width,
        trace_->reduced_before.height,
        trace_->reduced_before.format,
        bound_resource,
        bound_width,
        bound_height,
        bound_format,
        trace_->before_captured ? "captured" : "unavailable");
}

void RenderDebug::scaleform_trace_after_main_draw(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const reduced)
{
    if (trace_ == nullptr || !trace_->active) {
        return;
    }
    trace_->after_main_draw = true;
    trace_->after_captured =
        capture_surface(
            device,
            context,
            reduced,
            trace_->reduced_after,
            "reduced post-MainDraw");

    auto bound_target_difference = DifferenceResult{};
    if (trace_->main_draw_target_before_captured &&
        trace_->main_draw_target != nullptr &&
        capture_surface(
            device,
            context,
            trace_->main_draw_target.Get(),
            trace_->main_draw_target_after,
            "bound post-MainDraw")) {
        bound_target_difference = compare_surfaces(
            context,
            trace_->main_draw_target_before,
            trace_->main_draw_target_after);
    }

    unsigned bound_width{};
    unsigned bound_height{};
    unsigned bound_format{};
    const void* bound_resource{};
    if (context != nullptr) {
        ComPtr<ID3D11RenderTargetView> bound;
        context->OMGetRenderTargets(1, bound.GetAddressOf(), nullptr);
        if (bound) {
            ComPtr<ID3D11Resource> resource;
            bound->GetResource(resource.GetAddressOf());
            bound_resource = static_cast<const void*>(resource.Get());
            ComPtr<ID3D11Texture2D> texture;
            if (resource && SUCCEEDED(resource.As(&texture))) {
                D3D11_TEXTURE2D_DESC description{};
                texture->GetDesc(&description);
                bound_width = description.Width;
                bound_height = description.Height;
                bound_format = static_cast<unsigned>(description.Format);
            }
        }
    }

    auto difference = DifferenceResult{};
    if (trace_->before_captured && trace_->after_captured) {
        difference =
            compare_surfaces(
                context,
                trace_->reduced_before,
                trace_->reduced_after);
    }
    if (!difference.valid) {
        logger::warn(
            "Scaleform trace: after MainDraw, bound-rtv-resource={} "
            "({}x{} format={}); reduced-proxy difference UNAVAILABLE",
            bound_resource,
            bound_width,
            bound_height,
            bound_format);
        return;
    }
    const auto percent =
        difference.total != 0U ?
            (static_cast<double>(difference.changed) * 100.0) /
                static_cast<double>(difference.total) :
            0.0;
    logger::info(
        "Scaleform trace: after MainDraw, bound-rtv-resource={} "
        "({}x{} format={}); reduced-proxy changed-pixels={} of {} ({:.4f}%), "
        "changed-bounds={},{}..{},{}, verdict={}",
        bound_resource,
        bound_width,
        bound_height,
        bound_format,
        difference.changed,
        difference.total,
        percent,
        difference.min_x,
        difference.min_y,
        difference.max_x,
        difference.max_y,
        difference.changed == 0U ?
            "maindraw-does-not-write-reduced-proxy" :
            "maindraw-writes-reduced-proxy");
    if (bound_target_difference.valid) {
        const auto bound_percent =
            bound_target_difference.total != 0U ?
                (static_cast<double>(bound_target_difference.changed) * 100.0) /
                    static_cast<double>(bound_target_difference.total) :
                0.0;
        logger::info(
            "Scaleform trace: bound MainDraw target {}x{} format={} changed "
            "across MainDraw: changed-pixels={} of {} ({:.4f}%), "
            "changed-bounds={},{}..{},{}, verdict={}",
            trace_->main_draw_target_after.width,
            trace_->main_draw_target_after.height,
            trace_->main_draw_target_after.format,
            bound_target_difference.changed,
            bound_target_difference.total,
            bound_percent,
            bound_target_difference.min_x,
            bound_target_difference.min_y,
            bound_target_difference.max_x,
            bound_target_difference.max_y,
            bound_target_difference.changed == 0U ?
                "maindraw-does-not-write-bound-target" :
                "maindraw-writes-bound-target");
    } else {
        logger::warn(
            "Scaleform trace: bound MainDraw target difference unavailable");
    }
}

void RenderDebug::scaleform_trace_report(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const reduced_after,
    ID3D11Texture2D* const hudless,
    ID3D11Texture2D* const native)
{
    if (trace_ == nullptr || !trace_->active) {
        return;
    }
    scaleform_trace_end_target_episode(context);
    trace_->active = false;

    for (std::size_t index = 0; index < trace_->target_count; ++index) {
        const auto& entry = trace_->targets[index];
        logger::info(
            "Scaleform trace: ordered target bind {} of {}: requested={} "
            "({}x{} format={}), applied={} ({}x{} format={}), redirected={}, "
            "viewport={:.0f}x{:.0f}, scissor={}x{}, api={}, phase={}",
            index + 1U,
            trace_->target_count,
            entry.requested,
            entry.requested_width,
            entry.requested_height,
            entry.requested_format,
            entry.applied,
            entry.applied_width,
            entry.applied_height,
            entry.applied_format,
            entry.redirected,
            entry.viewport_width,
            entry.viewport_height,
            entry.scissor_width,
            entry.scissor_height,
            entry.unordered_access_variant ? "OMSetRT+UAV" : "OMSetRT",
            entry.after_main_draw ? "after-MainDraw" : "before-MainDraw");
    }
    if (trace_->overflow_binds != 0U) {
        logger::info(
            "Scaleform trace: {} further render-target binds exceeded the "
            "record capacity",
            trace_->overflow_binds);
    }

    TraceSurface hudless_surface{};
    TraceSurface present_surface{};
    const auto hudless_ok =
        capture_surface(
            device, context, hudless, hudless_surface, "hudless at Present");
    const auto present_ok =
        capture_surface(
            device,
            context,
            reduced_after,
            present_surface,
            "reduced at Present");
    if (hudless_ok && present_ok) {
        const auto difference =
            compare_surfaces(context, hudless_surface, present_surface);
        if (difference.valid) {
            const auto percent =
                difference.total != 0U ?
                    (static_cast<double>(difference.changed) * 100.0) /
                        static_cast<double>(difference.total) :
                    0.0;
            logger::info(
                "Scaleform trace: reconstruction inputs at Present, "
                "hudless={} ({}x{}) vs reduced={} ({}x{}): changed-pixels={} "
                "of {} ({:.4f}%), changed-bounds={},{}..{},{}, verdict={}",
                static_cast<const void*>(hudless),
                hudless_surface.width,
                hudless_surface.height,
                static_cast<const void*>(reduced_after),
                present_surface.width,
                present_surface.height,
                difference.changed,
                difference.total,
                percent,
                difference.min_x,
                difference.min_y,
                difference.max_x,
                difference.max_y,
                difference.changed == 0U ?
                    "identical-inputs-reconstruction-is-a-no-op" :
                    "inputs-differ-reconstruction-has-a-delta");
        } else {
            logger::warn(
                "Scaleform trace: reconstruction inputs could not be compared "
                "(hudless {}x{}, reduced {}x{})",
                hudless_surface.width,
                hudless_surface.height,
                present_surface.width,
                present_surface.height);
        }
    } else {
        logger::warn(
            "Scaleform trace: reconstruction inputs unavailable "
            "(hudless={}, reduced={})",
            hudless_ok ? "ok" : "missing",
            present_ok ? "ok" : "missing");
    }

    if (trace_->native_before_ui.copy != nullptr) {
        TraceSurface native_now{};
        if (capture_surface(
                device, context, native, native_now, "native at Present")) {
            const auto difference =
                compare_surfaces(
                    context, trace_->native_before_ui, native_now);
            if (difference.valid) {
                const auto percent =
                    difference.total != 0U ?
                        (static_cast<double>(difference.changed) * 100.0) /
                            static_cast<double>(difference.total) :
                        0.0;
                logger::info(
                    "Scaleform trace: native output {}x{} changed since the "
                    "pre-UI resolve: changed-pixels={} of {} ({:.4f}%), "
                    "changed-bounds={},{}..{},{}, verdict={}",
                    native_now.width,
                    native_now.height,
                    difference.changed,
                    difference.total,
                    percent,
                    difference.min_x,
                    difference.min_y,
                    difference.max_x,
                    difference.max_y,
                    difference.changed == 0U ?
                        "native-target-has-no-post-resolve-contribution" :
                        "native-target-already-contains-ui-contribution");
            }
        }
    }

    logger::info("Scaleform write-target trace {} complete", trace_runs_);
}
}
