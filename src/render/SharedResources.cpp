#include "render/SharedResources.hpp"

#include "config/Settings.hpp"
#include "render/CameraData.hpp"
#include "render/D3D12Backend.hpp"
#include "render/MainDepthTracker.hpp"
#include "render/FsrFrameGeneration.hpp"
#include "render/PresentationBridge.hpp"
#include "render/XessFrameGeneration.hpp"

#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <string>
#include <cstdint>
#include <array>
#include <chrono>
#include <cmath>
#include <string_view>
#include <utility>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

class UniqueHandle final
{
public:
    UniqueHandle() = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle()
    {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept :
        handle_(std::exchange(other.handle_, nullptr))
    {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept
    {
        return handle_;
    }

    [[nodiscard]] HANDLE* put() noexcept
    {
        reset();
        return &handle_;
    }

    void reset(const HANDLE handle = nullptr) noexcept
    {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_{};
};

class ScopedOutputMergerTargets final
{
public:
    explicit ScopedOutputMergerTargets(
        ID3D11DeviceContext* context) noexcept :
        context_(context)
    {
        if (context_ == nullptr) {
            return;
        }
        std::array<ID3D11RenderTargetView*,
                   D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
        ID3D11DepthStencilView* depth{};
        context_->OMGetRenderTargets(
            static_cast<UINT>(targets.size()),
            targets.data(),
            &depth);
        for (std::size_t index = 0; index < targets.size(); ++index) {
            targets_[index].Attach(targets[index]);
        }
        depth_.Attach(depth);
    }

    ~ScopedOutputMergerTargets()
    {
        if (context_ == nullptr) {
            return;
        }
        std::array<ID3D11RenderTargetView*,
                   D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
        UINT count{};
        for (std::size_t index = 0; index < targets_.size(); ++index) {
            targets[index] = targets_[index].Get();
            if (targets[index] != nullptr) {
                count = static_cast<UINT>(index + 1);
            }
        }

        const auto restored_count =
            (std::min)(
                count,
                static_cast<UINT>(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT));
        context_->OMSetRenderTargets(
            restored_count,
            restored_count != 0 ? targets.data() : nullptr,
            depth_.Get());
    }

    ScopedOutputMergerTargets(const ScopedOutputMergerTargets&) = delete;
    ScopedOutputMergerTargets& operator=(
        const ScopedOutputMergerTargets&) = delete;

private:
    ID3D11DeviceContext* context_{};
    std::array<ComPtr<ID3D11RenderTargetView>,
               D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets_{};
    ComPtr<ID3D11DepthStencilView> depth_;
};

struct SourceTextures
{
    ID3D11Texture2D* color{};
    ID3D11Texture2D* motion_vectors{};
    ID3D11Texture2D* depth{};
    ID3D11ShaderResourceView* motion_vectors_view{};
    ID3D11ShaderResourceView* depth_view{};
    ID3D11ShaderResourceView* temporal_aa_mask_view{};
    ID3D11ShaderResourceView* normals_view{};
};

struct TemporalInputConstants
{
    std::uint32_t width{};
    std::uint32_t height{};

    std::uint32_t reversed_depth{};

    std::uint32_t dilation_mode{};

    float camera_near{};
    float camera_far{};
    float padding_two{};
    float padding_three{};
};

static_assert(sizeof(TemporalInputConstants) == 32);

struct SharedTexture
{
    ComPtr<ID3D11Texture2D> d3d11;
    ComPtr<ID3D12Resource> d3d12;
    UniqueHandle handle;
    D3D11_TEXTURE2D_DESC description{};
};

bool vendor_suspended_for_rebuild = false;

void suspend_vendor_generation_for_rebuild()
{
    if (vendor_suspended_for_rebuild) {
        return;
    }
    if (XessFrameGeneration::selected() &&
        XessFrameGeneration::instance().owns_presentation()) {
        XessFrameGeneration::instance().set_enabled(false);
        vendor_suspended_for_rebuild = true;
    } else if (
        FsrFrameGeneration::selected() &&
        FsrFrameGeneration::instance().owns_presentation()) {
        FsrFrameGeneration::instance().set_enabled(false);
        vendor_suspended_for_rebuild = true;
    }
    if (vendor_suspended_for_rebuild) {
        logger::info(
            "Vendor frame generation suspended while the shared textures it "
            "reads are torn down and rebuilt");
    }
}

void resume_vendor_generation_after_rebuild()
{
    if (!vendor_suspended_for_rebuild) {
        return;
    }
    vendor_suspended_for_rebuild = false;
    if (XessFrameGeneration::selected()) {
        XessFrameGeneration::instance().reset_history();
        XessFrameGeneration::instance().set_enabled(true);
    } else if (FsrFrameGeneration::selected()) {
        FsrFrameGeneration::instance().reset_history();
        FsrFrameGeneration::instance().set_enabled(true);
    }
    logger::info(
        "Vendor frame generation resumed after the shared textures were "
        "rebuilt; interpolation history was reset");
}

struct VendorInputFingerprint
{
    std::array<const void*, 4U> resources{};
    std::array<UINT, 4U> widths{};
    std::array<UINT, 4U> heights{};
    std::array<DXGI_FORMAT, 4U> formats{};

    [[nodiscard]] bool operator==(
        const VendorInputFingerprint& other) const noexcept
    {
        return resources == other.resources && widths == other.widths &&
            heights == other.heights && formats == other.formats;
    }
};

[[nodiscard]] VendorInputFingerprint fingerprint_of(
    const SharedTexture& hudless,
    const SharedTexture& ui,
    const SharedTexture& depth,
    const SharedTexture& motion) noexcept
{
    const std::array<const SharedTexture*, 4U> inputs{
        &hudless, &ui, &depth, &motion};
    VendorInputFingerprint print{};
    for (std::size_t index = 0U; index < inputs.size(); ++index) {
        print.resources[index] =
            static_cast<const void*>(inputs[index]->d3d12.Get());
        print.widths[index] = inputs[index]->description.Width;
        print.heights[index] = inputs[index]->description.Height;
        print.formats[index] = inputs[index]->description.Format;
    }
    return print;
}

[[nodiscard]] SourceTextures get_source_textures() noexcept
{
    const auto* renderer = RE::BSGraphics::Renderer::GetRendererData();
    if (renderer == nullptr) {
        return {};
    }

    const auto& motion =
        renderer->renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
    auto& depth_tracker = MainDepthTracker::instance();
    const auto& temporal_aa_mask =
        renderer->renderTargets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK];
    const auto& normals =
        renderer->renderTargets[
            RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK];
    return {
        renderer->renderTargets[RE::RENDER_TARGETS::kMAIN].texture,
        motion.texture,
        depth_tracker.texture(),
        motion.SRV,
        depth_tracker.shader_resource_view(),
        temporal_aa_mask.SRV,
        normals.SRV};
}

[[nodiscard]] bool compatible_descriptions(
    const D3D11_TEXTURE2D_DESC& left,
    const D3D11_TEXTURE2D_DESC& right) noexcept
{
    return left.Width == right.Width &&
           left.Height == right.Height &&
           left.MipLevels == right.MipLevels &&
           left.ArraySize == right.ArraySize &&
           left.Format == right.Format &&
           left.SampleDesc.Count == right.SampleDesc.Count &&
           left.SampleDesc.Quality == right.SampleDesc.Quality;
}

[[nodiscard]] bool create_shared_texture(
    ID3D11Device* d3d11_device,
    ID3D12Device* d3d12_device,
    ID3D11Texture2D* source,
    const std::string_view name,
    SharedTexture& destination,
    const D3D11_TEXTURE2D_DESC* override_description = nullptr)
{
    if (source == nullptr) {
        logger::error("Cannot share unavailable {} texture", name);
        return false;
    }

    if (override_description != nullptr) {
        destination.description = *override_description;
    } else {
        source->GetDesc(&destination.description);
    }
    destination.description.Usage = D3D11_USAGE_DEFAULT;
    destination.description.CPUAccessFlags = 0;
    destination.description.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED |
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    auto hr = d3d11_device->CreateTexture2D(
        &destination.description,
        nullptr,
        &destination.d3d11);
    if (FAILED(hr)) {
        logger::error(
            "CreateTexture2D for shared {} failed: 0x{:08X} (format={}, bind=0x{:X})",
            name,
            static_cast<unsigned>(hr),
            static_cast<unsigned>(destination.description.Format),
            destination.description.BindFlags);
        return false;
    }

    ComPtr<IDXGIResource1> dxgi_resource;
    hr = destination.d3d11.As(&dxgi_resource);
    if (FAILED(hr)) {
        logger::error("Shared {} does not expose IDXGIResource1: 0x{:08X}", name, static_cast<unsigned>(hr));
        return false;
    }

    hr = dxgi_resource->CreateSharedHandle(
        nullptr,
        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr,
        destination.handle.put());
    if (FAILED(hr)) {
        logger::error("CreateSharedHandle for {} failed: 0x{:08X}", name, static_cast<unsigned>(hr));
        return false;
    }

    hr = d3d12_device->OpenSharedHandle(
        destination.handle.get(),
        IID_PPV_ARGS(&destination.d3d12));
    if (FAILED(hr)) {
        logger::error("D3D12 OpenSharedHandle for {} failed: 0x{:08X}", name, static_cast<unsigned>(hr));
        return false;
    }

    const auto d3d12_description = destination.d3d12->GetDesc();
    if (d3d12_description.Width != destination.description.Width ||
        d3d12_description.Height != destination.description.Height ||
        d3d12_description.Format != destination.description.Format) {
        logger::error("D3D11/D3D12 shared {} descriptions do not match", name);
        return false;
    }

    return true;
}

[[nodiscard]] bool create_depth_converter(
    ID3D11Device* device,
    ID3D11Texture2D* source,
    SharedTexture& destination,
    ComPtr<ID3D11ShaderResourceView>& source_view,
    ComPtr<ID3D11UnorderedAccessView>& destination_view,
    ComPtr<ID3D11ComputeShader>& shader)
{
    D3D11_TEXTURE2D_DESC source_description{};
    source->GetDesc(&source_description);
    if (source_description.Format != DXGI_FORMAT_R24G8_TYPELESS) {
        logger::error(
            "Unsupported source depth format for conversion: {}",
            static_cast<unsigned>(source_description.Format));
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC source_view_description{};
    source_view_description.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    source_view_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    source_view_description.Texture2D.MostDetailedMip = 0;
    source_view_description.Texture2D.MipLevels = 1;
    auto hr = device->CreateShaderResourceView(
        source,
        &source_view_description,
        &source_view);
    if (FAILED(hr)) {
        logger::error("CreateShaderResourceView for depth conversion failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC destination_view_description{};
    destination_view_description.Format = DXGI_FORMAT_R32_FLOAT;
    destination_view_description.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    hr = device->CreateUnorderedAccessView(
        destination.d3d11.Get(),
        &destination_view_description,
        &destination_view);
    if (FAILED(hr)) {
        logger::error("CreateUnorderedAccessView for shared depth failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    constexpr std::string_view source_code = R"(
Texture2D<float> source_depth : register(t0);
RWTexture2D<float> output_depth : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 thread_id : SV_DispatchThreadID)
{
    uint width;
    uint height;
    source_depth.GetDimensions(width, height);
    if (thread_id.x < width && thread_id.y < height) {
        output_depth[thread_id.xy] = source_depth[thread_id.xy];
    }
}
)";

    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    hr = D3DCompile(
        source_code.data(),
        source_code.size(),
        "MFGDLSSDepthCopy",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
        0,
        &bytecode,
        &errors);
    if (FAILED(hr)) {
        const auto message = errors != nullptr
            ? std::string_view(
                  static_cast<const char*>(errors->GetBufferPointer()),
                  errors->GetBufferSize())
            : std::string_view{"no compiler diagnostics"};
        logger::error(
            "Depth conversion shader compilation failed: 0x{:08X}: {}",
            static_cast<unsigned>(hr),
            message);
        return false;
    }

    hr = device->CreateComputeShader(
        bytecode->GetBufferPointer(),
        bytecode->GetBufferSize(),
        nullptr,
        &shader);
    if (FAILED(hr)) {
        logger::error("CreateComputeShader for depth conversion failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

[[nodiscard]] bool create_temporal_input_encoder(
    ID3D11Device* device,
    SharedTexture& motion,
    SharedTexture& vendor_motion,
    SharedTexture& reactive,
    SharedTexture& transparency,
    ComPtr<ID3D11UnorderedAccessView>& motion_view,
    ComPtr<ID3D11UnorderedAccessView>& vendor_motion_view,
    ComPtr<ID3D11UnorderedAccessView>& reactive_view,
    ComPtr<ID3D11UnorderedAccessView>& transparency_view,
    ComPtr<ID3D11Buffer>& constants,
    ComPtr<ID3D11ComputeShader>& shader)
{
    auto hr = device->CreateUnorderedAccessView(
        motion.d3d11.Get(),
        nullptr,
        &motion_view);
    if (FAILED(hr)) {
        logger::error(
            "CreateUnorderedAccessView for DLSS motion vectors failed: "
            "0x{:08X}",
            static_cast<unsigned>(hr));
        return false;
    }
    hr = device->CreateUnorderedAccessView(
        vendor_motion.d3d11.Get(),
        nullptr,
        &vendor_motion_view);
    if (FAILED(hr)) {
        logger::error(
            "CreateUnorderedAccessView for vendor motion vectors failed: "
            "0x{:08X}",
            static_cast<unsigned>(hr));
        return false;
    }
    hr = device->CreateUnorderedAccessView(
        reactive.d3d11.Get(),
        nullptr,
        &reactive_view);
    if (FAILED(hr)) {
        logger::error(
            "CreateUnorderedAccessView for DLSS reactive mask failed: "
            "0x{:08X}",
            static_cast<unsigned>(hr));
        return false;
    }
    hr = device->CreateUnorderedAccessView(
        transparency.d3d11.Get(),
        nullptr,
        &transparency_view);
    if (FAILED(hr)) {
        logger::error(
            "CreateUnorderedAccessView for DLSS transparency mask failed: "
            "0x{:08X}",
            static_cast<unsigned>(hr));
        return false;
    }

    D3D11_BUFFER_DESC constant_description{};
    constant_description.ByteWidth = sizeof(TemporalInputConstants);
    constant_description.Usage = D3D11_USAGE_DEFAULT;
    constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device->CreateBuffer(
        &constant_description,
        nullptr,
        &constants);
    if (FAILED(hr)) {
        logger::error(
            "CreateBuffer for DLSS temporal constants failed: 0x{:08X}",
            static_cast<unsigned>(hr));
        return false;
    }

    constexpr std::string_view source_code = R"(
cbuffer valid_extent : register(b0)
{
    uint2 valid_size;

    uint  reversed_depth;

    uint  dilation_mode;
    float camera_near;
    float camera_far;
    float unused_padding_two;
    float unused_padding_three;
};

static const uint kDilationStandard = 0;
static const uint kDilationNearFade = 1;
static const uint kDilationOff      = 2;

static const float kNearDilationFadeDistance = 10240.0;

float linear_view_depth(float raw_depth)
{
    if (!(camera_far > camera_near) || !(camera_near > 0.0)) {
        return -1.0;
    }
    const float normalized =
        (reversed_depth != 0) ? (1.0 - raw_depth) : raw_depth;
    const float denominator =
        camera_far - normalized * (camera_far - camera_near);
    if (!(abs(denominator) > 1e-6)) {
        return camera_far;
    }
    return (camera_near * camera_far) / denominator;
}

Texture2D<float2> source_motion : register(t0);
Texture2D<float> source_depth : register(t1);
Texture2D<float2> temporal_aa_mask : register(t2);
Texture2D<float4> normals_water_mask : register(t3);

RWTexture2D<float2> output_motion : register(u0);
RWTexture2D<float> output_reactive : register(u1);
RWTexture2D<float> output_transparency : register(u2);
RWTexture2D<float2> output_vendor_motion : register(u3);

[numthreads(8, 8, 1)]
void main(uint3 thread_id : SV_DispatchThreadID)
{
    if (any(thread_id.xy >= valid_size)) {
        return;
    }

    const int2 pixel = int2(thread_id.xy);
    const float depth = source_depth.Load(int3(pixel, 0));
    const float2 local_motion = source_motion.Load(int3(pixel, 0));
    float2 stable_motion = local_motion;

    if (dilation_mode != kDilationOff) {

        const int radius = (dilation_mode == kDilationNearFade) ? 2 : 1;
        float nearest_depth = depth;
        float longest_motion_squared = dot(local_motion, local_motion);

        [unroll]
        for (int y = -2; y <= 2; ++y) {
            [unroll]
            for (int x = -2; x <= 2; ++x) {
                if (max(abs(x), abs(y)) > radius) {
                    continue;
                }
                const int2 sample_pixel = pixel + int2(x, y);
                if (any(sample_pixel < 0) ||
                    any(sample_pixel >= int2(valid_size))) {
                    continue;
                }
                const float sample_depth =
                    source_depth.Load(int3(sample_pixel, 0));

                if (dilation_mode == kDilationNearFade) {

                    const bool closer = (reversed_depth != 0)
                        ? (sample_depth > depth)
                        : (sample_depth < depth);
                    if (!closer) {
                        continue;
                    }
                    const float2 sample_motion =
                        source_motion.Load(int3(sample_pixel, 0));
                    const float sample_motion_squared =
                        dot(sample_motion, sample_motion);
                    if (sample_motion_squared <= longest_motion_squared) {
                        continue;
                    }
                    longest_motion_squared = sample_motion_squared;
                    stable_motion = sample_motion;
                } else {

                    const bool closer = (reversed_depth != 0)
                        ? (sample_depth > nearest_depth)
                        : (sample_depth < nearest_depth);
                    if (!closer) {
                        continue;
                    }
                    nearest_depth = sample_depth;
                    stable_motion =
                        source_motion.Load(int3(sample_pixel, 0));
                }
            }
        }

        if (dilation_mode == kDilationNearFade) {

            const float view_depth = linear_view_depth(depth);
            if (view_depth >= 0.0) {
                const float near_weight =
                    smoothstep(kNearDilationFadeDistance, 0.0, view_depth);
                stable_motion = lerp(stable_motion, local_motion, near_weight);
            }
        }
    }

    const float2 taa =
        temporal_aa_mask.Load(int3(pixel, 0));
    const float4 normals_water =
        normals_water_mask.Load(int3(pixel, 0));
    output_motion[pixel] = stable_motion;

    output_vendor_motion[pixel] = local_motion;

    const float history_reject = taa.y;
    const float current_colour_hint = taa.x * 0.1;

    output_reactive[pixel] =
        min(saturate(current_colour_hint + history_reject), 0.9);
    output_transparency[pixel] = min(saturate(normals_water.z), 0.9);
}
)";

    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    hr = D3DCompile(
        source_code.data(),
        source_code.size(),
        "MFGDLSS_TemporalInputs",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
        0,
        &bytecode,
        &errors);
    if (FAILED(hr)) {
        const auto message = errors != nullptr
            ? std::string_view(
                  static_cast<const char*>(errors->GetBufferPointer()),
                  errors->GetBufferSize())
            : std::string_view{"no compiler diagnostics"};
        logger::error(
            "DLSS temporal-input shader compilation failed: 0x{:08X}: {}",
            static_cast<unsigned>(hr),
            message);
        return false;
    }

    hr = device->CreateComputeShader(
        bytecode->GetBufferPointer(),
        bytecode->GetBufferSize(),
        nullptr,
        &shader);
    if (FAILED(hr)) {
        logger::error(
            "CreateComputeShader for DLSS temporal inputs failed: "
            "0x{:08X}",
            static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

[[nodiscard]] bool create_ui_capture_target(
    ID3D11Device* device,
    ID3D11Texture2D* target,
    ComPtr<ID3D11RenderTargetView>& target_view,
    ComPtr<ID3D11ShaderResourceView>& target_resource_view)
{
    D3D11_TEXTURE2D_DESC description{};
    target->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        (description.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) {
        logger::warn(
            "Direct UI capture is unavailable for format {} with bind flags "
            "0x{:X}",
            static_cast<unsigned>(description.Format),
            description.BindFlags);
        return false;
    }

    const auto hr = device->CreateRenderTargetView(
        target,
        nullptr,
        &target_view);
    if (FAILED(hr)) {
        logger::warn(
            "CreateRenderTargetView for direct UI capture failed; "
            "UI recomposition remains disabled: 0x{:08X}",
            static_cast<unsigned>(hr));
        return false;
    }
    const auto resource_result = device->CreateShaderResourceView(
        target,
        nullptr,
        &target_resource_view);
    if (FAILED(resource_result)) {
        logger::warn(
            "CreateShaderResourceView for direct UI capture failed; "
            "UI recomposition remains disabled: 0x{:08X}",
            static_cast<unsigned>(resource_result));
        return false;
    }
    return true;
}

[[nodiscard]] bool create_ui_depth_target(
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC& color_description,
    ComPtr<ID3D11Texture2D>& depth_texture,
    ComPtr<ID3D11DepthStencilView>& depth_view)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = color_description.Width;
    description.Height = color_description.Height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    description.SampleDesc = {1, 0};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    auto hr = device->CreateTexture2D(&description, nullptr, &depth_texture);
    if (SUCCEEDED(hr)) {
        D3D11_DEPTH_STENCIL_VIEW_DESC view_description{};
        view_description.Format = description.Format;
        view_description.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        view_description.Texture2D.MipSlice = 0;
        hr = device->CreateDepthStencilView(
            depth_texture.Get(),
            &view_description,
            &depth_view);
    }
    if (FAILED(hr)) {
        logger::warn(
            "The {}x{} UI depth/stencil companion could not be created; direct "
            "UI capture remains disabled: 0x{:08X}",
            description.Width,
            description.Height,
            static_cast<unsigned>(hr));
        depth_texture.Reset();
        depth_view.Reset();
        return false;
    }
    return true;
}

void convert_depth(
    ID3D11DeviceContext* context,
    ID3D11ComputeShader* shader,
    ID3D11ShaderResourceView* source,
    ID3D11UnorderedAccessView* destination,
    const D3D11_TEXTURE2D_DESC& description)
{
    ComPtr<ID3D11ComputeShader> previous_shader;
    ComPtr<ID3D11ShaderResourceView> previous_source;
    ComPtr<ID3D11UnorderedAccessView> previous_destination;
    ID3D11ComputeShader* previous_shader_pointer{};
    ID3D11ShaderResourceView* previous_source_pointer{};
    ID3D11UnorderedAccessView* previous_destination_pointer{};

    context->CSGetShader(&previous_shader_pointer, nullptr, nullptr);
    context->CSGetShaderResources(0, 1, &previous_source_pointer);
    context->CSGetUnorderedAccessViews(0, 1, &previous_destination_pointer);
    previous_shader.Attach(previous_shader_pointer);
    previous_source.Attach(previous_source_pointer);
    previous_destination.Attach(previous_destination_pointer);

    context->CSSetShader(shader, nullptr, 0);
    context->CSSetShaderResources(0, 1, &source);
    context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
    context->Dispatch(
        (description.Width + 7) / 8,
        (description.Height + 7) / 8,
        1);

    ID3D11ShaderResourceView* no_source{};
    ID3D11UnorderedAccessView* no_destination{};
    context->CSSetShaderResources(0, 1, &no_source);
    context->CSSetUnorderedAccessViews(0, 1, &no_destination, nullptr);
    previous_shader_pointer = previous_shader.Get();
    previous_source_pointer = previous_source.Get();
    previous_destination_pointer = previous_destination.Get();
    context->CSSetShader(previous_shader_pointer, nullptr, 0);
    context->CSSetShaderResources(0, 1, &previous_source_pointer);
    context->CSSetUnorderedAccessViews(
        0,
        1,
        &previous_destination_pointer,
        nullptr);
}

[[nodiscard]] bool encode_temporal_inputs(
    ID3D11DeviceContext* context,
    ID3D11ComputeShader* shader,
    ID3D11Buffer* constants,
    const std::array<ID3D11ShaderResourceView*, 4>& sources,
    const std::array<ID3D11UnorderedAccessView*, 4>& destinations,
    const std::uint32_t width,
    const std::uint32_t height,
    const bool reversed_depth)
{
    ComPtr<ID3D11ComputeShader> previous_shader;
    ComPtr<ID3D11Buffer> previous_constants;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> previous_sources;
    std::array<ComPtr<ID3D11UnorderedAccessView>, 4>
        previous_destinations;
    ID3D11ComputeShader* shader_pointer{};
    ID3D11Buffer* constants_pointer{};
    std::array<ID3D11ShaderResourceView*, 4> source_pointers{};
    std::array<ID3D11UnorderedAccessView*, 4> destination_pointers{};

    context->CSGetShader(&shader_pointer, nullptr, nullptr);
    context->CSGetConstantBuffers(0, 1, &constants_pointer);
    context->CSGetShaderResources(
        0,
        static_cast<UINT>(source_pointers.size()),
        source_pointers.data());
    context->CSGetUnorderedAccessViews(
        0,
        static_cast<UINT>(destination_pointers.size()),
        destination_pointers.data());
    previous_shader.Attach(shader_pointer);
    previous_constants.Attach(constants_pointer);
    for (std::size_t index = 0; index < source_pointers.size(); ++index) {
        previous_sources[index].Attach(source_pointers[index]);
    }
    for (
        std::size_t index = 0;
        index < destination_pointers.size();
        ++index) {
        previous_destinations[index].Attach(destination_pointers[index]);
    }

    float camera_near{};
    float camera_far{};
    static_cast<void>(
        CameraData::instance().camera_planes(camera_near, camera_far));

    {
        const auto dilation = config::Settings::instance().motion_dilation();
        static auto logged_dilation = config::MotionDilation::off;
        static bool dilation_logged = false;
        if (!dilation_logged || logged_dilation != dilation) {
            dilation_logged = true;
            logged_dilation = dilation;
            logger::info(
                "Motion dilation rule: {}",
                dilation == config::MotionDilation::standard ?
                    "Standard (3x3, nearest surface wins)" :
                    dilation == config::MotionDilation::near_fade ?
                        "NearFade (5x5, nearer and faster, 10240-unit fade)" :
                        "Off (raw per-pixel velocity)");
        }
    }

    const TemporalInputConstants input_constants{
        width,
        height,
        reversed_depth ? 1U : 0U,
        static_cast<std::uint32_t>(
            config::Settings::instance().motion_dilation()),
        camera_near,
        camera_far,
        0.0F,
        0.0F};
    context->UpdateSubresource(
        constants,
        0,
        nullptr,
        &input_constants,
        0,
        0);
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &constants);
    context->CSSetShaderResources(
        0,
        static_cast<UINT>(sources.size()),
        sources.data());
    context->CSSetUnorderedAccessViews(
        0,
        static_cast<UINT>(destinations.size()),
        destinations.data(),
        nullptr);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    const std::array<ID3D11ShaderResourceView*, 4> no_sources{};
    const std::array<ID3D11UnorderedAccessView*, 4> no_destinations{};
    context->CSSetShaderResources(
        0,
        static_cast<UINT>(no_sources.size()),
        no_sources.data());
    context->CSSetUnorderedAccessViews(
        0,
        static_cast<UINT>(no_destinations.size()),
        no_destinations.data(),
        nullptr);

    shader_pointer = previous_shader.Get();
    constants_pointer = previous_constants.Get();
    for (std::size_t index = 0; index < source_pointers.size(); ++index) {
        source_pointers[index] = previous_sources[index].Get();
    }
    for (
        std::size_t index = 0;
        index < destination_pointers.size();
        ++index) {
        destination_pointers[index] = previous_destinations[index].Get();
    }
    context->CSSetShader(shader_pointer, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &constants_pointer);
    context->CSSetShaderResources(
        0,
        static_cast<UINT>(source_pointers.size()),
        source_pointers.data());
    context->CSSetUnorderedAccessViews(
        0,
        static_cast<UINT>(destination_pointers.size()),
        destination_pointers.data(),
        nullptr);
    return true;
}

}

struct SharedResources::State
{
    ComPtr<ID3D11Device5> d3d11_device;
    ComPtr<ID3D11DeviceContext4> d3d11_context;
    ComPtr<ID3D12Device> d3d12_device;
    ComPtr<ID3D12CommandQueue> d3d12_queue;
    ComPtr<ID3D11Fence> d3d11_ready_fence;
    ComPtr<ID3D12Fence> d3d12_ready_fence;
    UniqueHandle ready_fence_handle;
    SharedTexture color;
    SharedTexture complete_frame;
    SharedTexture frame_generation_hudless;
    SharedTexture motion_vectors;
    SharedTexture upscaling_motion_vectors;
    SharedTexture vendor_motion_vectors;
    SharedTexture depth;
    SharedTexture reactive_mask;
    SharedTexture transparency_mask;
    SharedTexture upscaled_output;
    SharedTexture ui;
    VendorInputFingerprint vendor_input_fingerprint{};
    bool vendor_input_logged{};
    ComPtr<ID3D11ShaderResourceView> depth_source_view;
    ComPtr<ID3D11UnorderedAccessView> depth_destination_view;
    ComPtr<ID3D11ComputeShader> depth_converter;
    ComPtr<ID3D11UnorderedAccessView>
        upscaling_motion_vectors_destination_view;
    ComPtr<ID3D11UnorderedAccessView> vendor_motion_destination_view;
    ComPtr<ID3D11UnorderedAccessView> reactive_mask_destination_view;
    ComPtr<ID3D11UnorderedAccessView>
        transparency_mask_destination_view;
    ComPtr<ID3D11Buffer> temporal_input_constants;
    ComPtr<ID3D11ComputeShader> temporal_input_encoder;
    ComPtr<ID3D11RenderTargetView> ui_draw_target_view;
    ComPtr<ID3D11ShaderResourceView> ui_draw_target_resource_view;
    ComPtr<ID3D11Texture2D> ui_depth;
    ComPtr<ID3D11DepthStencilView> ui_depth_view;
    std::uint64_t ready_value{};
    bool ui_rendering{};
    bool first_copy_logged{};
    bool first_frame_generation_hudless_logged{};
    bool missing_frame_generation_hudless_logged{};
    bool frame_generation_hudless_mismatch_logged{};
    bool first_temporal_input_logged{};
    bool first_upscaler_handoff_logged{};
    bool first_ui_capture_logged{};
};

SharedResources& SharedResources::instance() noexcept
{
    static SharedResources resources;
    return resources;
}

bool SharedResources::initialize()
{
    if (ready()) {
        return true;
    }

    auto& presentation = PresentationBridge::instance();
    auto& backend = D3D12Backend::instance();
    const auto sources = get_source_textures();
    auto* hudless_source =
        presentation.uses_virtual_render_surface() ?
            presentation.d3d11_render_buffer() :
            presentation.d3d11_back_buffer();
    auto* output_source = presentation.d3d11_back_buffer();
    if (!presentation.ready() || !backend.ready() ||
        hudless_source == nullptr ||
        output_source == nullptr ||
        sources.motion_vectors == nullptr ||
        sources.depth == nullptr) {
        return false;
    }

    auto state = std::make_unique<State>();
    auto hr = presentation.d3d11_device()->QueryInterface(
        IID_PPV_ARGS(&state->d3d11_device));
    if (FAILED(hr)) {
        logger::error("ID3D11Device5 is required for cross-API fence sharing: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    hr = presentation.d3d11_context()->QueryInterface(
        IID_PPV_ARGS(&state->d3d11_context));
    if (FAILED(hr)) {
        logger::error("ID3D11DeviceContext4 is required for cross-API fence sharing: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    state->d3d12_device =
        static_cast<ID3D12Device*>(backend.native_device());
    state->d3d12_queue =
        static_cast<ID3D12CommandQueue*>(backend.command_queue());
    if (state->d3d12_device == nullptr || state->d3d12_queue == nullptr) {
        return false;
    }

    hr = state->d3d12_device->CreateFence(
        0,
        D3D12_FENCE_FLAG_SHARED,
        IID_PPV_ARGS(&state->d3d12_ready_fence));
    if (FAILED(hr)) {
        logger::error("D3D12 CreateFence for shared frames failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    hr = state->d3d12_device->CreateSharedHandle(
        state->d3d12_ready_fence.Get(),
        nullptr,
        GENERIC_ALL,
        nullptr,
        state->ready_fence_handle.put());
    if (FAILED(hr)) {
        logger::error("CreateSharedHandle for frame fence failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    hr = state->d3d11_device->OpenSharedFence(
        state->ready_fence_handle.get(),
        IID_PPV_ARGS(&state->d3d11_ready_fence));
    if (FAILED(hr)) {
        logger::error("D3D11 OpenSharedFence failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC color_description{};
    hudless_source->GetDesc(&color_description);
    color_description.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    D3D11_TEXTURE2D_DESC frame_generation_color_description{};
    output_source->GetDesc(&frame_generation_color_description);
    frame_generation_color_description.BindFlags |=
        D3D11_BIND_SHADER_RESOURCE;

    auto upscaled_output_description = frame_generation_color_description;
    upscaled_output_description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE |
        D3D11_BIND_UNORDERED_ACCESS |
        D3D11_BIND_RENDER_TARGET;

    if (!create_shared_texture(
            state->d3d11_device.Get(),
            state->d3d12_device.Get(),
            hudless_source,
            "post-processed HUD-less color",
            state->color,
            &color_description) ||
        !create_shared_texture(
            state->d3d11_device.Get(),
            state->d3d12_device.Get(),
            output_source,
            "full-resolution completed presentation color",
            state->complete_frame,
            &frame_generation_color_description) ||
        !create_shared_texture(
            state->d3d11_device.Get(),
            state->d3d12_device.Get(),
            output_source,
            "full-resolution frame-generation HUD-less color",
            state->frame_generation_hudless,
            &frame_generation_color_description) ||
        !create_shared_texture(
            state->d3d11_device.Get(),
            state->d3d12_device.Get(),
            sources.motion_vectors,
            "motion-vector",
            state->motion_vectors)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC depth_description{};
    sources.depth->GetDesc(&depth_description);
    depth_description.Format = DXGI_FORMAT_R32_FLOAT;
    depth_description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE |
        D3D11_BIND_UNORDERED_ACCESS;
    depth_description.MipLevels = 1;
    depth_description.ArraySize = 1;
    depth_description.SampleDesc = {1, 0};

    if (!create_shared_texture(
            state->d3d11_device.Get(),
            state->d3d12_device.Get(),
            sources.depth,
            "depth",
            state->depth,
            &depth_description) ||
        !create_depth_converter(
            state->d3d11_device.Get(),
            sources.depth,
            state->depth,
            state->depth_source_view,
            state->depth_destination_view,
            state->depth_converter)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC temporal_motion_description{};
    sources.motion_vectors->GetDesc(&temporal_motion_description);
    temporal_motion_description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE |
        D3D11_BIND_UNORDERED_ACCESS;
    temporal_motion_description.MipLevels = 1;
    temporal_motion_description.ArraySize = 1;
    temporal_motion_description.SampleDesc = {1, 0};

    auto temporal_mask_description = temporal_motion_description;
    temporal_mask_description.Format = DXGI_FORMAT_R8_UNORM;
    if (!create_shared_texture(
            state->d3d11_device.Get(),
            state->d3d12_device.Get(),
            sources.motion_vectors,
            "DLSS dilated motion-vector",
            state->upscaling_motion_vectors,
            &temporal_motion_description) ||
        !create_shared_texture(
            state->d3d11_device.Get(),
            state->d3d12_device.Get(),
            sources.motion_vectors,
            "vendor undilated motion-vector",
            state->vendor_motion_vectors,
            &temporal_motion_description) ||
        !create_shared_texture(
            state->d3d11_device.Get(),
            state->d3d12_device.Get(),
            sources.motion_vectors,
            "DLSS reactive mask",
            state->reactive_mask,
            &temporal_mask_description) ||
        !create_shared_texture(
            state->d3d11_device.Get(),
            state->d3d12_device.Get(),
            sources.motion_vectors,
            "DLSS transparency mask",
            state->transparency_mask,
            &temporal_mask_description) ||
        !create_temporal_input_encoder(
            state->d3d11_device.Get(),
            state->upscaling_motion_vectors,
            state->vendor_motion_vectors,
            state->reactive_mask,
            state->transparency_mask,
            state->upscaling_motion_vectors_destination_view,
            state->vendor_motion_destination_view,
            state->reactive_mask_destination_view,
            state->transparency_mask_destination_view,
            state->temporal_input_constants,
            state->temporal_input_encoder)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC ui_description{};
    output_source->GetDesc(&ui_description);
    if (ui_description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        static auto ui_format_override_logged = false;
        if (!ui_format_override_logged) {
            ui_format_override_logged = true;
            logger::warn(
                "The presentation surface is format {}, but this plugin's UI "
                "layer is created as R8G8B8A8_UNORM (28) because the UI "
                "extraction and composition shaders are written for it. The "
                "HUD-less and completed-frame surfaces keep the presentation "
                "format, so they will not match the UI layer and UI extraction "
                "will refuse, which suspends frame generation.",
                static_cast<unsigned>(ui_description.Format));
        }
    }
    ui_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ui_description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE |
        D3D11_BIND_RENDER_TARGET;
    ui_description.MipLevels = 1;
    ui_description.ArraySize = 1;
    ui_description.SampleDesc = {1, 0};
    const auto ui_ready =
        create_shared_texture(
            state->d3d11_device.Get(),
            state->d3d12_device.Get(),
            output_source,
            "UI color and alpha",
            state->ui,
            &ui_description) &&
        create_ui_capture_target(
            state->d3d11_device.Get(),
            state->ui.d3d11.Get(),
            state->ui_draw_target_view,
            state->ui_draw_target_resource_view) &&
        create_ui_depth_target(
            state->d3d11_device.Get(),
            ui_description,
            state->ui_depth,
            state->ui_depth_view);

    const auto upscaled_output_ready = create_shared_texture(
        state->d3d11_device.Get(),
        state->d3d12_device.Get(),
        output_source,
        "vendor upscaled output",
        state->upscaled_output,
        &upscaled_output_description);
    if (!upscaled_output_ready) {
        logger::warn(
            "The shared D3D12 upscaler output is unavailable; DLSS remains "
            "active but Intel XeSS and AMD FSR are disabled");
    }

    logger::info(
        "Cross-API resources ready: color={} complete={} fg-hudless={} "
        "motion={} DLSS-motion={} vendor-motion={} "
        "depth={} reactive={} transparency={} vendor-output={} ui={} "
        "ui-recomposition={} fence={}",
        static_cast<void*>(state->color.d3d12.Get()),
        static_cast<void*>(state->complete_frame.d3d12.Get()),
        static_cast<void*>(
            state->frame_generation_hudless.d3d12.Get()),
        static_cast<void*>(state->motion_vectors.d3d12.Get()),
        static_cast<void*>(
            state->upscaling_motion_vectors.d3d12.Get()),
        static_cast<void*>(state->vendor_motion_vectors.d3d12.Get()),
        static_cast<void*>(state->depth.d3d12.Get()),
        static_cast<void*>(state->reactive_mask.d3d12.Get()),
        static_cast<void*>(state->transparency_mask.d3d12.Get()),
        static_cast<void*>(state->upscaled_output.d3d12.Get()),
        static_cast<void*>(state->ui.d3d12.Get()),
        ui_ready,
        static_cast<void*>(state->d3d12_ready_fence.Get()));

    state_ = std::move(state);
    resume_vendor_generation_after_rebuild();
    return true;
}

void SharedResources::begin_frame() noexcept
{

    end_ui_rendering();
    hudless_captured_ = false;
    frame_generation_hudless_captured_ = false;
    complete_frame_captured_ = false;
    ui_captured_ = false;
    temporal_inputs_prepared_ = false;
    temporal_inputs_synchronized_ = false;
    prepared_width_ = 0;
    prepared_height_ = 0;
}

bool SharedResources::capture_hudless(ID3D11Texture2D* source)
{
    if (source == nullptr) {
        return false;
    }
    if (!ready()) {
        if (retry_frames_remaining_ != 0) {
            return false;
        }
        if (!initialize()) {
            retry_frames_remaining_ = 300;
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC current{};
    source->GetDesc(&current);
    if (!compatible_descriptions(current, state_->color.description)) {
        logger::info(
            "HUD-less source changed; suspending this frame and rebuilding "
            "next frame");
        shutdown();
        return false;
    }

    state_->d3d11_context->CopyResource(state_->color.d3d11.Get(), source);
    if (compatible_descriptions(
            current,
            state_->frame_generation_hudless.description)) {
        state_->d3d11_context->CopyResource(
            state_->frame_generation_hudless.d3d11.Get(),
            source);
        frame_generation_hudless_captured_ = true;
        state_->missing_frame_generation_hudless_logged = false;
    } else if (!state_->frame_generation_hudless_mismatch_logged) {
        state_->frame_generation_hudless_mismatch_logged = true;
        logger::warn(
            "HUD-less source {}x{} format {} does not match the "
            "frame-generation HUD-less surface {}x{} format {}, so this "
            "capture is SILENTLY SKIPPED and frame generation suspends every "
            "frame from here. The sibling check ten lines above rebuilds on a "
            "mismatch; this one never did, which is why the suspension has no "
            "recovery path",
            current.Width,
            current.Height,
            static_cast<std::uint32_t>(current.Format),
            state_->frame_generation_hudless.description.Width,
            state_->frame_generation_hudless.description.Height,
            static_cast<std::uint32_t>(
                state_->frame_generation_hudless.description.Format));
    }
    const std::array transparent{0.0F, 0.0F, 0.0F, 0.0F};
    state_->d3d11_context->ClearRenderTargetView(
        state_->ui_draw_target_view.Get(),
        transparent.data());
    hudless_captured_ = true;
    return true;
}

bool SharedResources::capture_frame_generation_hudless(
    ID3D11Texture2D* source)
{
    if (source == nullptr) {
        return false;
    }
    if (!ready()) {
        if (retry_frames_remaining_ != 0) {
            return false;
        }
        if (!initialize()) {
            retry_frames_remaining_ = 300;
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC current{};
    source->GetDesc(&current);
    if (!compatible_descriptions(
            current,
            state_->frame_generation_hudless.description)) {
        logger::warn(
            "Full-resolution HUD-less source extent changed from {}x{} "
            "to {}x{}; suspending this frame and rebuilding next frame",
            state_->frame_generation_hudless.description.Width,
            state_->frame_generation_hudless.description.Height,
            current.Width,
            current.Height);
        shutdown();
        return false;
    }

    const ScopedOutputMergerTargets restore_targets{
        state_->d3d11_context.Get()};
    state_->d3d11_context->OMSetRenderTargets(0, nullptr, nullptr);
    state_->d3d11_context->CopyResource(
        state_->frame_generation_hudless.d3d11.Get(),
        source);
    frame_generation_hudless_captured_ = true;
    state_->missing_frame_generation_hudless_logged = false;
    if (!state_->first_frame_generation_hudless_logged) {
        state_->first_frame_generation_hudless_logged = true;
        logger::info(
            "First full-resolution frame-generation HUD-less color "
            "captured at {}x{}",
            current.Width,
            current.Height);
    }
    return true;
}

bool SharedResources::begin_ui_rendering()
{
    if (!ui_recomposition_available() ||
        !hudless_captured_ ||
        state_->ui_rendering) {
        return false;
    }

    state_->ui_rendering = true;

    const std::array transparent{0.0F, 0.0F, 0.0F, 0.0F};
    state_->d3d11_context->ClearRenderTargetView(
        state_->ui_draw_target_view.Get(),
        transparent.data());

    state_->d3d11_context->ClearDepthStencilView(
        state_->ui_depth_view.Get(),
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
        1.0F,
        0);
    rebind_ui_target();
    return true;
}

void SharedResources::rebind_ui_target() noexcept
{
    if (state_ == nullptr || !state_->ui_rendering) {
        return;
    }
    auto* ui_view = state_->ui_draw_target_view.Get();
    state_->d3d11_context->OMSetRenderTargets(
        1,
        &ui_view,
        state_->ui_depth_view.Get());

    const auto width = state_->ui.description.Width;
    const auto height = state_->ui.description.Height;
    const D3D11_VIEWPORT viewport{
        0.0F,
        0.0F,
        static_cast<float>(width),
        static_cast<float>(height),
        0.0F,
        1.0F};
    const D3D11_RECT scissor{
        0,
        0,
        static_cast<LONG>(width),
        static_cast<LONG>(height)};
    state_->d3d11_context->RSSetViewports(1, &viewport);
    state_->d3d11_context->RSSetScissorRects(1, &scissor);
}

void SharedResources::end_ui_rendering() noexcept
{
    if (state_ == nullptr || !state_->ui_rendering) {
        return;
    }

    state_->d3d11_context->OMSetRenderTargets(0, nullptr, nullptr);
    state_->ui_rendering = false;

    if (!state_->first_ui_capture_logged) {
        state_->first_ui_capture_logged = true;
        logger::info(
            "First native-resolution transparent UI layer captured at "
            "{}x{}",
            state_->ui.description.Width,
            state_->ui.description.Height);
    }
}

bool SharedResources::ui_rendering() const noexcept
{
    return state_ != nullptr && state_->ui_rendering;
}

ID3D11RenderTargetView* SharedResources::ui_draw_target_view() const noexcept
{
    return state_ != nullptr ? state_->ui_draw_target_view.Get() : nullptr;
}

ID3D11DepthStencilView* SharedResources::ui_depth_view() const noexcept
{
    return state_ != nullptr ? state_->ui_depth_view.Get() : nullptr;
}

bool SharedResources::capture_ui(ID3D11Texture2D* final_color)
{
    if (final_color == nullptr ||
        !ui_recomposition_available() ||
        !hudless_captured_) {
        return false;
    }
    if (ui_captured_) {
        return true;
    }

    D3D11_TEXTURE2D_DESC current{};
    final_color->GetDesc(&current);
    if (!compatible_descriptions(current, state_->color.description) ||
        current.Width != state_->ui.description.Width ||
        current.Height != state_->ui.description.Height) {
        logger::info(
            "Final color target changed; suspending this frame and rebuilding "
            "next frame");
        shutdown();
        return false;
    }

    state_->d3d11_context->OMSetRenderTargets(0, nullptr, nullptr);
    state_->d3d11_context->CopyResource(
        state_->ui.d3d11.Get(),
        final_color);
    ui_captured_ = true;
    if (!state_->first_ui_capture_logged) {
        state_->first_ui_capture_logged = true;
        logger::info(
            "First transparent UI layer captured for native recomposition");
    }
    return true;
}

bool SharedResources::copy_frame()
{

    using StallClock = std::chrono::steady_clock;
    const auto stage_start = StallClock::now();
    auto motion_copy_done = stage_start;
    auto hudless_copy_done = stage_start;
    auto complete_copy_done = stage_start;
    auto fence_signal_done = stage_start;
    auto fence_wait_done = stage_start;
    static StallClock::time_point last_stall_report{};

    if (!ready()) {
        if (retry_frames_remaining_ != 0) {
            --retry_frames_remaining_;
            return false;
        }
        if (!initialize()) {
            retry_frames_remaining_ = 300;
            return false;
        }
    }

    const auto sources = get_source_textures();

    auto* const source_motion = sources.motion_vectors;
    if (source_motion == nullptr) {
        shutdown();
        return false;
    }
    D3D11_TEXTURE2D_DESC current_motion_description{};
    source_motion->GetDesc(&current_motion_description);
    if (!compatible_descriptions(
            current_motion_description,
            state_->motion_vectors.description)) {
        logger::warn(
            "Renderer motion-vector target changed; suspending this "
            "frame and rebuilding next frame");
        shutdown();
        return false;
    }
    state_->d3d11_context->CopyResource(
        state_->motion_vectors.d3d11.Get(),
        source_motion);
    motion_copy_done = StallClock::now();

    if (!hudless_captured_) {
        auto& presentation = PresentationBridge::instance();
        auto* fallback =
            presentation.uses_virtual_render_surface() ?
                presentation.d3d11_render_buffer() :
                presentation.d3d11_back_buffer();
        if (fallback == nullptr) {
            fallback = sources.color;
        }
        if (fallback == nullptr) {
            shutdown();
            return false;
        }
        D3D11_TEXTURE2D_DESC current{};
        fallback->GetDesc(&current);
        if (!compatible_descriptions(current, state_->color.description)) {
            logger::warn(
                "HUD-less fallback extent no longer matches the render "
                "surface; suspending this frame and rebuilding next frame");
            shutdown();
            return false;
        }
        state_->d3d11_context->CopyResource(
            state_->color.d3d11.Get(),
            fallback);
        hudless_captured_ = true;
    }
    hudless_copy_done = StallClock::now();

    if (!frame_generation_hudless_captured_) {
        if (!state_->missing_frame_generation_hudless_logged) {
            state_->missing_frame_generation_hudless_logged = true;
            logger::warn(
                "MFG suspended: no full-resolution pre-UI HUD-less color "
                "was captured for this frame");
        }
        return false;
    }

    if (!streamline_ui_recomposition_available()) {
        auto* complete_frame =
            PresentationBridge::instance().d3d11_back_buffer();
        if (complete_frame == nullptr) {
            logger::warn(
                "MFG suspended: the completed presentation frame is unavailable");
            return false;
        }
        D3D11_TEXTURE2D_DESC complete_frame_description{};
        complete_frame->GetDesc(&complete_frame_description);
        if (!compatible_descriptions(
                complete_frame_description,
                state_->complete_frame.description)) {
            logger::warn(
                "Completed presentation extent changed; suspending this frame "
                "and rebuilding next frame");
            shutdown();
            return false;
        }
        state_->d3d11_context->CopyResource(
            state_->complete_frame.d3d11.Get(),
            complete_frame);
        complete_frame_captured_ = true;
    }
    complete_copy_done = StallClock::now();

    ++state_->ready_value;
    auto hr = state_->d3d11_context->Signal(
        state_->d3d11_ready_fence.Get(),
        state_->ready_value);
    if (FAILED(hr)) {
        logger::error("D3D11 frame-fence signal failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }
    fence_signal_done = StallClock::now();

    hr = state_->d3d12_queue->Wait(
        state_->d3d12_ready_fence.Get(),
        state_->ready_value);
    if (FAILED(hr)) {
        logger::error("D3D12 frame-fence wait failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }
    fence_wait_done = StallClock::now();
    if (temporal_inputs_prepared_) {
        temporal_inputs_synchronized_ = true;
    }

    if (!state_->first_copy_logged) {
        state_->first_copy_logged = true;
        logger::info(
            "First late frame-generation color/UI capture synchronized at "
            "fence {} (raw-motion-copy={}, complete-frame-copy={})",
            state_->ready_value,
            true,
            complete_frame_captured_);
    }

    const auto stage_end = StallClock::now();
    const auto milliseconds =
        [](const StallClock::time_point from,
           const StallClock::time_point to) {
            return std::chrono::duration<double, std::milli>(to - from)
                .count();
        };
    if (const auto total = milliseconds(stage_start, stage_end);
        total >= 8.0 &&
        (last_stall_report == StallClock::time_point{} ||
         stage_end - last_stall_report >= std::chrono::seconds(1))) {
        last_stall_report = stage_end;
        logger::warn(
            "Capture stall {:.2f}ms, split: raw-motion copy {:.2f}, HUD-less "
            "copy {:.2f}, complete-frame copy {:.2f}, D3D11 fence signal "
            "{:.2f}, D3D12 queue-wait enqueue {:.2f}, bookkeeping {:.2f}. "
            "Every GPU operation here is nominally asynchronous, so "
            "whichever dominates is where the CPU blocked inside the driver.",
            total,
            milliseconds(stage_start, motion_copy_done),
            milliseconds(motion_copy_done, hudless_copy_done),
            milliseconds(hudless_copy_done, complete_copy_done),
            milliseconds(complete_copy_done, fence_signal_done),
            milliseconds(fence_signal_done, fence_wait_done),
            milliseconds(fence_wait_done, stage_end));
    }
    return true;
}

bool SharedResources::prepare_temporal_inputs(
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const bool synchronize_for_d3d12)
{
    if (!ready() || render_width == 0 || render_height == 0 ||
        render_width > state_->upscaling_motion_vectors.description.Width ||
        render_height >
            state_->upscaling_motion_vectors.description.Height ||
        render_width > state_->vendor_motion_vectors.description.Width ||
        render_height > state_->vendor_motion_vectors.description.Height) {
        return false;
    }

    const auto synchronize = [this]() {
        ++state_->ready_value;
        auto hr = state_->d3d11_context->Signal(
            state_->d3d11_ready_fence.Get(),
            state_->ready_value);
        if (FAILED(hr)) {
            logger::error(
                "D3D11 temporal-input fence signal failed: 0x{:08X}",
                static_cast<unsigned>(hr));
            return false;
        }
        hr = state_->d3d12_queue->Wait(
            state_->d3d12_ready_fence.Get(),
            state_->ready_value);
        if (FAILED(hr)) {
            logger::error(
                "D3D12 temporal-input fence wait failed: 0x{:08X}",
                static_cast<unsigned>(hr));
            return false;
        }
        temporal_inputs_synchronized_ = true;
        return true;
    };

    if (temporal_inputs_prepared_ &&
        prepared_width_ == render_width &&
        prepared_height_ == render_height) {
        return !synchronize_for_d3d12 ||
               temporal_inputs_synchronized_ || synchronize();
    }

    const auto sources = get_source_textures();
    if (sources.motion_vectors_view == nullptr ||
        sources.depth_view == nullptr) {
        logger::error(
            "Skyrim motion-vector or main-depth SRV is unavailable for "
            "temporal reconstruction");
        return false;
    }

    const ScopedOutputMergerTargets restore_targets{
        state_->d3d11_context.Get()};
    state_->d3d11_context->OMSetRenderTargets(0, nullptr, nullptr);

    const std::array input_views{
        sources.motion_vectors_view,
        sources.depth_view,
        sources.temporal_aa_mask_view,
        sources.normals_view};
    const std::array output_views{
        state_->upscaling_motion_vectors_destination_view.Get(),
        state_->reactive_mask_destination_view.Get(),
        state_->transparency_mask_destination_view.Get(),
        state_->vendor_motion_destination_view.Get()};
    if (!encode_temporal_inputs(
            state_->d3d11_context.Get(),
            state_->temporal_input_encoder.Get(),
            state_->temporal_input_constants.Get(),
            input_views,
            output_views,
            render_width,
            render_height,
            CameraData::instance().depth_reversed_for_display())) {
        return false;
    }

    convert_depth(
        state_->d3d11_context.Get(),
        state_->depth_converter.Get(),
        state_->depth_source_view.Get(),
        state_->depth_destination_view.Get(),
        state_->depth.description);

    if (!state_->first_temporal_input_logged) {
        state_->first_temporal_input_logged = true;
        const auto dilation =
            config::Settings::instance().motion_dilation();
        const auto* const dilation_description =
            dilation == config::MotionDilation::standard ?
                "3x3 nearest-surface dilation" :
            dilation == config::MotionDilation::near_fade ?
                "5x5 nearer-and-faster dilation with 10240-unit near fade" :
                "raw per-pixel vectors (dilation disabled)";
        logger::info(
            "Temporal inputs prepared at {}x{}: current main depth, "
            "DLSS motion={}, depth={}, plus undilated vendor motion, "
            "reactive-mask={}, "
            "transparency-mask={}",
            render_width,
            render_height,
            dilation_description,
            describe(CameraData::instance().depth_orientation()),
            sources.temporal_aa_mask_view != nullptr,
            sources.normals_view != nullptr);
    }
    temporal_inputs_prepared_ = true;
    temporal_inputs_synchronized_ = false;
    prepared_width_ = render_width;
    prepared_height_ = render_height;
    return !synchronize_for_d3d12 || synchronize();
}

bool SharedResources::complete_upscaler_dispatch()
{
    if (!ready() || state_->upscaled_output.d3d11 == nullptr ||
        state_->upscaled_output.d3d12 == nullptr) {
        return false;
    }

    ++state_->ready_value;
    auto hr = state_->d3d12_queue->Signal(
        state_->d3d12_ready_fence.Get(),
        state_->ready_value);
    if (FAILED(hr)) {
        logger::error(
            "D3D12 upscaler-output fence signal failed: 0x{:08X}",
            static_cast<unsigned>(hr));
        return false;
    }

    hr = state_->d3d11_context->Wait(
        state_->d3d11_ready_fence.Get(),
        state_->ready_value);
    if (FAILED(hr)) {
        logger::error(
            "D3D11 upscaler-output fence wait failed: 0x{:08X}",
            static_cast<unsigned>(hr));
        return false;
    }

    if (!state_->first_upscaler_handoff_logged) {
        state_->first_upscaler_handoff_logged = true;
        logger::info(
            "First D3D12 upscaler output synchronized back to D3D11 at "
            "fence {} ({}x{}, format={})",
            state_->ready_value,
            state_->upscaled_output.description.Width,
            state_->upscaled_output.description.Height,
            static_cast<unsigned>(state_->upscaled_output.description.Format));
    }
    return true;
}

void SharedResources::shutdown() noexcept
{
    suspend_vendor_generation_for_rebuild();
    end_ui_rendering();
    state_.reset();
    retry_frames_remaining_ = 0;
    prepared_width_ = 0;
    prepared_height_ = 0;
    hudless_captured_ = false;
    frame_generation_hudless_captured_ = false;
    complete_frame_captured_ = false;
    ui_captured_ = false;
    temporal_inputs_prepared_ = false;
    temporal_inputs_synchronized_ = false;
}

bool SharedResources::ready() const noexcept
{
    return state_ != nullptr;
}

void* SharedResources::color() const noexcept
{
    return state_ != nullptr ? state_->color.d3d12.Get() : nullptr;
}

void* SharedResources::complete_frame() const noexcept
{
    return state_ != nullptr ? state_->complete_frame.d3d12.Get() : nullptr;
}

void* SharedResources::frame_generation_hudless() const noexcept
{
    return state_ != nullptr ?
               state_->frame_generation_hudless.d3d12.Get() :
               nullptr;
}

void* SharedResources::motion_vectors() const noexcept
{
    return state_ != nullptr ? state_->motion_vectors.d3d12.Get() : nullptr;
}

void* SharedResources::upscaling_motion_vectors() const noexcept
{
    return state_ != nullptr ?
               state_->upscaling_motion_vectors.d3d12.Get() :
               nullptr;
}

void* SharedResources::vendor_motion_vectors() const noexcept
{
    return state_ != nullptr ?
               state_->vendor_motion_vectors.d3d12.Get() :
               nullptr;
}

void* SharedResources::depth() const noexcept
{
    return state_ != nullptr ? state_->depth.d3d12.Get() : nullptr;
}

void* SharedResources::reactive_mask() const noexcept
{
    return state_ != nullptr ? state_->reactive_mask.d3d12.Get() : nullptr;
}

void* SharedResources::transparency_mask() const noexcept
{
    return state_ != nullptr ?
               state_->transparency_mask.d3d12.Get() :
               nullptr;
}

void* SharedResources::upscaled_output() const noexcept
{
    return state_ != nullptr ? state_->upscaled_output.d3d12.Get() : nullptr;
}

void SharedResources::log_vendor_input_provenance(
    const char* const consumer) noexcept
{
    if (state_ == nullptr) {
        return;
    }
    const auto print = fingerprint_of(
        state_->frame_generation_hudless,
        state_->ui,
        state_->depth,
        state_->vendor_motion_vectors);
    if (state_->vendor_input_logged &&
        print == state_->vendor_input_fingerprint) {
        return;
    }
    state_->vendor_input_logged = true;
    state_->vendor_input_fingerprint = print;
    logger::info(
        "{} frame generation inputs: hudless {} {}x{} fmt{}, ui {} {}x{} "
        "fmt{}, depth {} {}x{} fmt{}, motion {} {}x{} fmt{}. A motion or "
        "depth extent of 0x0 means nothing has written them, which is what "
        "an interpolated but motionless image looks like.",
        consumer,
        static_cast<void*>(state_->frame_generation_hudless.d3d12.Get()),
        state_->frame_generation_hudless.description.Width,
        state_->frame_generation_hudless.description.Height,
        static_cast<unsigned>(
            state_->frame_generation_hudless.description.Format),
        static_cast<void*>(state_->ui.d3d12.Get()),
        state_->ui.description.Width,
        state_->ui.description.Height,
        static_cast<unsigned>(state_->ui.description.Format),
        static_cast<void*>(state_->depth.d3d12.Get()),
        state_->depth.description.Width,
        state_->depth.description.Height,
        static_cast<unsigned>(state_->depth.description.Format),
        static_cast<void*>(state_->vendor_motion_vectors.d3d12.Get()),
        state_->vendor_motion_vectors.description.Width,
        state_->vendor_motion_vectors.description.Height,
        static_cast<unsigned>(
            state_->vendor_motion_vectors.description.Format));
}

void* SharedResources::ui_color_alpha() const noexcept
{
    return ui_recomposition_available() ?
               state_->ui.d3d12.Get() :
               nullptr;
}

ID3D11Texture2D* SharedResources::hudless_color_d3d11() const noexcept
{
    return state_ != nullptr && hudless_captured_ ?
               state_->color.d3d11.Get() :
               nullptr;
}

ID3D11Texture2D*
SharedResources::frame_generation_hudless_d3d11() const noexcept
{
    return state_ != nullptr && frame_generation_hudless_captured_ ?
               state_->frame_generation_hudless.d3d11.Get() :
               nullptr;
}

ID3D11Texture2D* SharedResources::ui_color_alpha_d3d11() const noexcept
{
    return ui_recomposition_available() ?
               state_->ui.d3d11.Get() :
               nullptr;
}

ID3D11Texture2D* SharedResources::temporal_motion_d3d11() const noexcept
{
    return state_ != nullptr ?
               state_->upscaling_motion_vectors.d3d11.Get() :
               nullptr;
}

ID3D11Texture2D* SharedResources::vendor_motion_d3d11() const noexcept
{
    return state_ != nullptr ?
               state_->vendor_motion_vectors.d3d11.Get() :
               nullptr;
}

ID3D11Texture2D* SharedResources::generator_motion_d3d11() const noexcept
{
    return state_ != nullptr ? state_->motion_vectors.d3d11.Get() : nullptr;
}

ID3D11Texture2D* SharedResources::depth_d3d11() const noexcept
{
    return state_ != nullptr ? state_->depth.d3d11.Get() : nullptr;
}

ID3D11Texture2D* SharedResources::reactive_mask_d3d11() const noexcept
{
    return state_ != nullptr ?
               state_->reactive_mask.d3d11.Get() :
               nullptr;
}

ID3D11Texture2D*
SharedResources::transparency_mask_d3d11() const noexcept
{
    return state_ != nullptr ?
               state_->transparency_mask.d3d11.Get() :
               nullptr;
}

ID3D11Texture2D* SharedResources::upscaled_output_d3d11() const noexcept
{
    return state_ != nullptr ? state_->upscaled_output.d3d11.Get() : nullptr;
}

std::uint32_t SharedResources::color_format() const noexcept
{
    return state_ != nullptr ?
               static_cast<std::uint32_t>(
                   state_->frame_generation_hudless.description.Format) :
               0;
}

std::uint32_t SharedResources::ui_format() const noexcept
{
    return ui_recomposition_available() ?
               static_cast<std::uint32_t>(state_->ui.description.Format) :
               0;
}

bool SharedResources::ui_recomposition_available() const noexcept
{
    return state_ != nullptr &&
           state_->ui.d3d12 != nullptr &&
           state_->ui.d3d11 != nullptr &&
           state_->ui_draw_target_view != nullptr &&
           state_->ui_draw_target_resource_view != nullptr &&
           state_->ui_depth_view != nullptr;
}

bool SharedResources::streamline_ui_recomposition_available()
    const noexcept
{
    return ui_recomposition_available() && ui_captured_;
}

bool SharedResources::hudless_captured() const noexcept
{
    return hudless_captured_;
}

bool SharedResources::frame_generation_hudless_captured() const noexcept
{
    return frame_generation_hudless_captured_;
}

bool SharedResources::temporal_inputs_prepared() const noexcept
{
    return temporal_inputs_prepared_;
}

bool SharedResources::complete_frame_captured() const noexcept
{
    return complete_frame_captured_;
}

bool SharedResources::ui_captured() const noexcept
{
    return ui_captured_;
}

void SharedResources::mark_ui_captured() noexcept
{
    ui_captured_ = ui_recomposition_available();
    if (ui_captured_ && !state_->first_ui_capture_logged) {
        state_->first_ui_capture_logged = true;
        logger::info(
            "First {}x{} premultiplied UI color/alpha layer prepared "
            "for DLSS-G recomposition",
            state_->ui.description.Width,
            state_->ui.description.Height);
    }
}

std::uint64_t SharedResources::ready_fence_value() const noexcept
{
    return state_ != nullptr ? state_->ready_value : 0;
}
}
