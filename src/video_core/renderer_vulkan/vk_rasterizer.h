// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "core/hw/gpu.h"
#include "video_core/rasterizer_accelerated.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_renderpass_cache.h"
#include "video_core/renderer_vulkan/vk_stream_buffer.h"
#include "video_core/renderer_vulkan/vk_texture_runtime.h"

namespace Frontend {
class EmuWindow;
}

namespace Vulkan {

struct ScreenInfo;

class Instance;
class Scheduler;
class RenderpassCache;
class DescriptorManager;

struct SamplerInfo {
    using TextureConfig = Pica::TexturingRegs::TextureConfig;
    TextureConfig::TextureFilter mag_filter;
    TextureConfig::TextureFilter min_filter;
    TextureConfig::TextureFilter mip_filter;
    TextureConfig::WrapMode wrap_s;
    TextureConfig::WrapMode wrap_t;
    u32 border_color = 0;
    float lod_min = 0;
    float lod_max = 0;

    // TODO(wwylele): remove this once mipmap for cube is implemented
    bool supress_mipmap_for_cube = false;

    auto operator<=>(const SamplerInfo&) const noexcept = default;
};

struct FramebufferInfo {
    vk::ImageView color;
    vk::ImageView depth;
    vk::RenderPass renderpass;
    u32 width = 1;
    u32 height = 1;

    auto operator<=>(const FramebufferInfo&) const noexcept = default;
};

} // namespace Vulkan

namespace std {
template <>
struct hash<Vulkan::SamplerInfo> {
    std::size_t operator()(const Vulkan::SamplerInfo& info) const noexcept {
        return Common::ComputeHash64(&info, sizeof(Vulkan::SamplerInfo));
    }
};

template <>
struct hash<Vulkan::FramebufferInfo> {
    std::size_t operator()(const Vulkan::FramebufferInfo& info) const noexcept {
        return Common::ComputeHash64(&info, sizeof(Vulkan::FramebufferInfo));
    }
};
} // namespace std

namespace Vulkan {

class RasterizerVulkan : public VideoCore::RasterizerAccelerated {
    friend class RendererVulkan;

public:
    explicit RasterizerVulkan(Frontend::EmuWindow& emu_window, const Instance& instance,
                              Scheduler& scheduler, DescriptorManager& desc_manager,
                              TextureRuntime& runtime, RenderpassCache& renderpass_cache);
    ~RasterizerVulkan() override;

    void LoadDiskResources(const std::atomic_bool& stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback) override;

    void DrawTriangles() override;
    void FlushAll() override;
    void FlushRegion(PAddr addr, u32 size) override;
    void InvalidateRegion(PAddr addr, u32 size) override;
    void FlushAndInvalidateRegion(PAddr addr, u32 size) override;
    bool AccelerateDisplayTransfer(const GPU::Regs::DisplayTransferConfig& config) override;
    bool AccelerateTextureCopy(const GPU::Regs::DisplayTransferConfig& config) override;
    bool AccelerateFill(const GPU::Regs::MemoryFillConfig& config) override;
    bool AccelerateDisplay(const GPU::Regs::FramebufferConfig& config, PAddr framebuffer_addr,
                           u32 pixel_stride, ScreenInfo& screen_info);
    bool AccelerateDrawBatch(bool is_indexed) override;

    void SyncFixedState() override;

private:
    void NotifyFixedFunctionPicaRegisterChanged(u32 id) override;

    /// Syncs the clip enabled status to match the PICA register
    void SyncClipEnabled();

    /// Syncs the cull mode to match the PICA register
    void SyncCullMode();

    /// Syncs the blend enabled status to match the PICA register
    void SyncBlendEnabled();

    /// Syncs the blend functions to match the PICA register
    void SyncBlendFuncs();

    /// Syncs the blend color to match the PICA register
    void SyncBlendColor();

    /// Syncs the logic op states to match the PICA register
    void SyncLogicOp();

    /// Syncs the color write mask to match the PICA register state
    void SyncColorWriteMask();

    /// Syncs the stencil write mask to match the PICA register state
    void SyncStencilWriteMask();

    /// Syncs the depth write mask to match the PICA register state
    void SyncDepthWriteMask();

    /// Syncs the stencil test states to match the PICA register
    void SyncStencilTest();

    /// Syncs the depth test states to match the PICA register
    void SyncDepthTest();

    /// Syncs and uploads the lighting, fog and proctex LUTs
    void SyncAndUploadLUTs();
    void SyncAndUploadLUTsLF();

    /// Upload the uniform blocks to the uniform buffer object
    void UploadUniforms(bool accelerate_draw);

    /// Generic draw function for DrawTriangles and AccelerateDrawBatch
    bool Draw(bool accelerate, bool is_indexed);

    /// Internal implementation for AccelerateDrawBatch
    bool AccelerateDrawBatchInternal(bool is_indexed);

    /// Setup index array for AccelerateDrawBatch
    void SetupIndexArray();

    /// Setup vertex array for AccelerateDrawBatch
    void SetupVertexArray(u32 vs_input_size, u32 vs_input_index_min, u32 vs_input_index_max);

    /// Setup the fixed attribute emulation in vulkan
    void SetupFixedAttribs();

    /// Setup vertex shader for AccelerateDrawBatch
    bool SetupVertexShader();

    /// Setup geometry shader for AccelerateDrawBatch
    bool SetupGeometryShader();

    /// Creates the vertex layout struct used for software shader pipelines
    void MakeSoftwareVertexLayout();

    /// Creates a new sampler object
    vk::Sampler CreateSampler(const SamplerInfo& info);

    /// Creates a new Vulkan framebuffer object
    vk::Framebuffer CreateFramebuffer(const FramebufferInfo& info);

private:
    const Instance& instance;
    Scheduler& scheduler;
    TextureRuntime& runtime;
    RenderpassCache& renderpass_cache;
    DescriptorManager& desc_manager;
    RasterizerCache res_cache;
    PipelineCache pipeline_cache;

    VertexLayout software_layout;
    std::array<u64, 16> binding_offsets{};
    std::array<bool, 16> enable_attributes{};
    std::array<vk::Buffer, 16> vertex_buffers;
    vk::Sampler default_sampler;
    Surface null_surface;
    Surface null_storage_surface;

    std::array<SamplerInfo, 3> texture_samplers;
    SamplerInfo texture_cube_sampler;
    std::unordered_map<SamplerInfo, vk::Sampler> samplers;
    std::unordered_map<FramebufferInfo, vk::Framebuffer> framebuffers;
    PipelineInfo pipeline_info;

    StreamBuffer stream_buffer;     ///< Vertex+Index+Uniform buffer
    StreamBuffer texture_buffer;    ///< Texture buffer
    StreamBuffer texture_lf_buffer; ///< Texture Light-Fog buffer
    vk::BufferView texture_lf_view;
    vk::BufferView texture_rg_view;
    vk::BufferView texture_rgba_view;
    u64 uniform_buffer_alignment;
    u64 uniform_size_aligned_vs;
    u64 uniform_size_aligned_fs;
};

} // namespace Vulkan
