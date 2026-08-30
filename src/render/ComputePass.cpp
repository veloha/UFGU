#include "render/ComputePass.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#include <array>
#include <vector>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

[[nodiscard]] D3D12_STATIC_SAMPLER_DESC clamped_sampler(
    const D3D12_FILTER filter,
    const UINT shader_register) noexcept
{
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = filter;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0F;
    sampler.MaxAnisotropy = 1U;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0F;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = shader_register;
    sampler.RegisterSpace = 0U;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return sampler;
}
}

struct ComputePass::State
{
    std::string name;
    std::string detail;
    ComputeBindingLayout layout{};
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline_state;
    ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    ID3D12Device* device{};
    UINT descriptor_size{};
    UINT table_parameter{};
    UINT constant_parameter{};
    bool has_table{};
    bool has_constants{};
};

ComputePass::ComputePass() : state_{std::make_unique<State>()} {}

ComputePass::~ComputePass()
{
    destroy();
}

bool ComputePass::ready() const noexcept
{
    return state_->pipeline_state != nullptr;
}

const std::string& ComputePass::detail() const noexcept
{
    return state_->detail;
}

const ComputeBindingLayout& ComputePass::layout() const noexcept
{
    return state_->layout;
}

void ComputePass::destroy() noexcept
{
    state_->descriptor_heap.Reset();
    state_->pipeline_state.Reset();
    state_->root_signature.Reset();
    state_->device = nullptr;
    state_->descriptor_size = 0U;
    state_->has_table = false;
    state_->has_constants = false;
}

bool ComputePass::create(
    const std::string_view name,
    const std::string_view hlsl,
    const std::string_view entry_point,
    const ComputeBindingLayout& binding_layout,
    void* d3d12_device)
{
    destroy();
    state_->name.assign(name);
    state_->layout = binding_layout;
    state_->detail.clear();

    if (!compute_binding_layout_is_valid(binding_layout)) {
        state_->detail =
            "the binding layout is not one the runtime could create";
        logger::warn(
            "Compute pass '{}' was not created: {}",
            state_->name,
            state_->detail);
        return false;
    }

    auto* device = static_cast<ID3D12Device*>(d3d12_device);
    if (device == nullptr) {
        state_->detail = "the D3D12 device is not available";
        logger::warn(
            "Compute pass '{}' was not created: {}",
            state_->name,
            state_->detail);
        return false;
    }
    state_->device = device;

    const std::string entry{entry_point};
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    const auto compiled = D3DCompile(
        hlsl.data(),
        hlsl.size(),
        state_->name.c_str(),
        nullptr,
        nullptr,
        entry.c_str(),
        "cs_5_1",
        flags,
        0U,
        bytecode.GetAddressOf(),
        errors.GetAddressOf());
    if (FAILED(compiled)) {
        state_->detail = "the compute shader did not compile";
        if (errors != nullptr && errors->GetBufferPointer() != nullptr) {
            state_->detail += ": ";
            state_->detail.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        logger::error(
            "Compute pass '{}' was not created: {}",
            state_->name,
            state_->detail);
        destroy();
        return false;
    }

    std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
    UINT range_count = 0U;
    if (binding_layout.shader_resources != 0U) {
        auto& range = ranges.at(range_count);
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = binding_layout.shader_resources;
        range.BaseShaderRegister = 0U;
        range.RegisterSpace = 0U;
        range.OffsetInDescriptorsFromTableStart =
            compute_shader_resource_slot(binding_layout, 0U);
        ++range_count;
    }
    if (binding_layout.unordered_access != 0U) {
        auto& range = ranges.at(range_count);
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = binding_layout.unordered_access;
        range.BaseShaderRegister = 0U;
        range.RegisterSpace = 0U;
        range.OffsetInDescriptorsFromTableStart =
            compute_unordered_access_slot(binding_layout, 0U);
        ++range_count;
    }

    std::vector<D3D12_ROOT_PARAMETER> parameters;
    if (range_count != 0U) {
        D3D12_ROOT_PARAMETER table{};
        table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        table.DescriptorTable.NumDescriptorRanges = range_count;
        table.DescriptorTable.pDescriptorRanges = ranges.data();
        state_->table_parameter = static_cast<UINT>(parameters.size());
        state_->has_table = true;
        parameters.push_back(table);
    }
    if (binding_layout.root_constant_dwords != 0U) {
        D3D12_ROOT_PARAMETER constants{};
        constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        constants.Constants.ShaderRegister = 0U;
        constants.Constants.RegisterSpace = 0U;
        constants.Constants.Num32BitValues =
            binding_layout.root_constant_dwords;
        state_->constant_parameter = static_cast<UINT>(parameters.size());
        state_->has_constants = true;
        parameters.push_back(constants);
    }

    std::array<D3D12_STATIC_SAMPLER_DESC, kComputeMaxStaticSamplers> samplers{};
    if (binding_layout.static_samplers >= 1U) {
        samplers.at(0) =
            clamped_sampler(D3D12_FILTER_MIN_MAG_MIP_POINT, 0U);
    }
    if (binding_layout.static_samplers >= 2U) {
        samplers.at(1) =
            clamped_sampler(D3D12_FILTER_MIN_MAG_MIP_LINEAR, 1U);
    }

    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = static_cast<UINT>(parameters.size());
    signature.pParameters = parameters.empty() ? nullptr : parameters.data();
    signature.NumStaticSamplers = binding_layout.static_samplers;
    signature.pStaticSamplers =
        binding_layout.static_samplers == 0U ? nullptr : samplers.data();
    signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> signature_errors;
    const auto signed_result = D3D12SerializeRootSignature(
        &signature,
        D3D_ROOT_SIGNATURE_VERSION_1,
        serialized.GetAddressOf(),
        signature_errors.GetAddressOf());
    if (FAILED(signed_result)) {
        state_->detail = "the root signature could not be serialised";
        if (signature_errors != nullptr &&
            signature_errors->GetBufferPointer() != nullptr) {
            state_->detail += ": ";
            state_->detail.append(
                static_cast<const char*>(signature_errors->GetBufferPointer()),
                signature_errors->GetBufferSize());
        }
        logger::error(
            "Compute pass '{}' was not created: {}",
            state_->name,
            state_->detail);
        destroy();
        return false;
    }

    const auto root_created = device->CreateRootSignature(
        0U,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(state_->root_signature.GetAddressOf()));
    if (FAILED(root_created)) {
        state_->detail = "the root signature could not be created";
        logger::error(
            "Compute pass '{}' was not created: {}",
            state_->name,
            state_->detail);
        destroy();
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = state_->root_signature.Get();
    pipeline.CS.pShaderBytecode = bytecode->GetBufferPointer();
    pipeline.CS.BytecodeLength = bytecode->GetBufferSize();
    pipeline.NodeMask = 0U;
    pipeline.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    const auto pipeline_created = device->CreateComputePipelineState(
        &pipeline, IID_PPV_ARGS(state_->pipeline_state.GetAddressOf()));
    if (FAILED(pipeline_created)) {
        state_->detail = "the compute pipeline state could not be created";
        logger::error(
            "Compute pass '{}' was not created: {}",
            state_->name,
            state_->detail);
        destroy();
        return false;
    }

    const auto descriptors = compute_descriptor_count(binding_layout);
    if (descriptors != 0U) {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = descriptors;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heap.NodeMask = 0U;
        const auto heap_created = device->CreateDescriptorHeap(
            &heap, IID_PPV_ARGS(state_->descriptor_heap.GetAddressOf()));
        if (FAILED(heap_created)) {
            state_->detail = "the descriptor heap could not be created";
            logger::error(
                "Compute pass '{}' was not created: {}",
                state_->name,
                state_->detail);
            destroy();
            return false;
        }
        state_->descriptor_size = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    logger::info(
        "Compute pass '{}' is ready: {} shader resource(s), {} output(s), {} "
        "root constant DWORD(s), {} static sampler(s), {} of the 64 root "
        "signature DWORDs used",
        state_->name,
        binding_layout.shader_resources,
        binding_layout.unordered_access,
        binding_layout.root_constant_dwords,
        binding_layout.static_samplers,
        compute_root_signature_dwords(binding_layout));
    return true;
}

bool ComputePass::bind_shader_resource(
    const std::uint32_t index,
    void* d3d12_resource,
    const std::uint32_t format_override)
{
    const auto slot = compute_shader_resource_slot(state_->layout, index);
    if (slot == kComputeInvalidSlot) {
        state_->detail = "shader resource index is outside the declared layout";
        return false;
    }
    if (!ready() || state_->descriptor_heap == nullptr ||
        d3d12_resource == nullptr) {
        state_->detail = "the pass or the resource is not available to bind";
        return false;
    }

    auto* resource = static_cast<ID3D12Resource*>(d3d12_resource);
    const auto description = resource->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        state_->detail = "only two dimensional textures can be bound";
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format_override == 0U
        ? description.Format
        : static_cast<DXGI_FORMAT>(format_override);
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MostDetailedMip = 0U;
    view.Texture2D.MipLevels = description.MipLevels;
    view.Texture2D.PlaneSlice = 0U;
    view.Texture2D.ResourceMinLODClamp = 0.0F;

    auto handle =
        state_->descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * state_->descriptor_size;
    state_->device->CreateShaderResourceView(resource, &view, handle);
    return true;
}

bool ComputePass::bind_unordered_access(
    const std::uint32_t index,
    void* d3d12_resource,
    const std::uint32_t format_override)
{
    const auto slot = compute_unordered_access_slot(state_->layout, index);
    if (slot == kComputeInvalidSlot) {
        state_->detail = "output index is outside the declared layout";
        return false;
    }
    if (!ready() || state_->descriptor_heap == nullptr ||
        d3d12_resource == nullptr) {
        state_->detail = "the pass or the resource is not available to bind";
        return false;
    }

    auto* resource = static_cast<ID3D12Resource*>(d3d12_resource);
    const auto description = resource->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        state_->detail = "only two dimensional textures can be bound";
        return false;
    }
    if ((description.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ==
        D3D12_RESOURCE_FLAG_NONE) {
        state_->detail =
            "the texture was not created with unordered access allowed";
        return false;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = format_override == 0U
        ? description.Format
        : static_cast<DXGI_FORMAT>(format_override);
    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipSlice = 0U;
    view.Texture2D.PlaneSlice = 0U;

    auto handle =
        state_->descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * state_->descriptor_size;
    state_->device->CreateUnorderedAccessView(
        resource, nullptr, &view, handle);
    return true;
}

bool ComputePass::bind_unordered_access_buffer(
    const std::uint32_t index,
    void* d3d12_resource,
    const std::uint32_t byte_size)
{
    const auto slot = compute_unordered_access_slot(state_->layout, index);
    if (slot == kComputeInvalidSlot) {
        state_->detail = "output index is outside the declared layout";
        return false;
    }
    if (!ready() || state_->descriptor_heap == nullptr ||
        d3d12_resource == nullptr || byte_size < 4U) {
        state_->detail = "the pass or the buffer is not available to bind";
        return false;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    view.Buffer.FirstElement = 0ULL;
    view.Buffer.NumElements = byte_size / 4U;
    view.Buffer.StructureByteStride = 0U;
    view.Buffer.CounterOffsetInBytes = 0ULL;
    view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

    auto handle =
        state_->descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * state_->descriptor_size;
    state_->device->CreateUnorderedAccessView(
        static_cast<ID3D12Resource*>(d3d12_resource), nullptr, &view, handle);
    return true;
}

bool ComputePass::dispatch(
    void* d3d12_graphics_command_list,
    const ComputeDispatchGroups& groups,
    const void* root_constants,
    const std::uint32_t root_constant_dwords)
{
    if (!ready() || d3d12_graphics_command_list == nullptr) {
        state_->detail = "the pass is not ready to dispatch";
        return false;
    }
    if (!compute_dispatch_groups_are_valid(groups)) {
        state_->detail = "the dispatch would cover no pixels at all";
        return false;
    }
    if (state_->has_constants) {
        if (root_constants == nullptr ||
            root_constant_dwords != state_->layout.root_constant_dwords) {
            state_->detail =
                "the root constants supplied do not match the declared layout";
            return false;
        }
    }

    auto* commands =
        static_cast<ID3D12GraphicsCommandList*>(d3d12_graphics_command_list);
    commands->SetComputeRootSignature(state_->root_signature.Get());
    if (state_->has_table) {
        std::array<ID3D12DescriptorHeap*, 1> heaps{
            state_->descriptor_heap.Get()};
        commands->SetDescriptorHeaps(
            static_cast<UINT>(heaps.size()), heaps.data());
        commands->SetComputeRootDescriptorTable(
            state_->table_parameter,
            state_->descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    }
    if (state_->has_constants) {
        commands->SetComputeRoot32BitConstants(
            state_->constant_parameter,
            root_constant_dwords,
            root_constants,
            0U);
    }
    commands->SetPipelineState(state_->pipeline_state.Get());
    commands->Dispatch(groups.x, groups.y, groups.z);
    return true;
}
}
