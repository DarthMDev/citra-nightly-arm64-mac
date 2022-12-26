// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include "common/bit_field.h"
#include "common/hash.h"
#include "video_core/rasterizer_cache/pixel_format.h"
#include "video_core/regs.h"
#include "video_core/renderer_vulkan/vk_shader_gen_spv.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/shader/shader_cache.h"

namespace Vulkan {

constexpr u32 MAX_SHADER_STAGES = 3;
constexpr u32 MAX_VERTEX_ATTRIBUTES = 16;
constexpr u32 MAX_VERTEX_BINDINGS = 16;

/**
 * The pipeline state is tightly packed with bitfields to reduce
 * the overhead of hashing as much as possible
 */
union RasterizationState {
    u8 value = 0;
    BitField<0, 2, Pica::PipelineRegs::TriangleTopology> topology;
    BitField<4, 2, Pica::RasterizerRegs::CullMode> cull_mode;
};

union DepthStencilState {
    u32 value = 0;
    BitField<0, 1, u32> depth_test_enable;
    BitField<1, 1, u32> depth_write_enable;
    BitField<2, 1, u32> stencil_test_enable;
    BitField<3, 3, Pica::FramebufferRegs::CompareFunc> depth_compare_op;
    BitField<6, 3, Pica::FramebufferRegs::StencilAction> stencil_fail_op;
    BitField<9, 3, Pica::FramebufferRegs::StencilAction> stencil_pass_op;
    BitField<12, 3, Pica::FramebufferRegs::StencilAction> stencil_depth_fail_op;
    BitField<15, 3, Pica::FramebufferRegs::CompareFunc> stencil_compare_op;
};

union BlendingState {
    u32 value = 0;
    BitField<0, 1, u32> blend_enable;
    BitField<1, 4, Pica::FramebufferRegs::BlendFactor> src_color_blend_factor;
    BitField<5, 4, Pica::FramebufferRegs::BlendFactor> dst_color_blend_factor;
    BitField<9, 3, Pica::FramebufferRegs::BlendEquation> color_blend_eq;
    BitField<12, 4, Pica::FramebufferRegs::BlendFactor> src_alpha_blend_factor;
    BitField<16, 4, Pica::FramebufferRegs::BlendFactor> dst_alpha_blend_factor;
    BitField<20, 3, Pica::FramebufferRegs::BlendEquation> alpha_blend_eq;
    BitField<23, 4, u32> color_write_mask;
    BitField<27, 4, Pica::FramebufferRegs::LogicOp> logic_op;
};

struct DynamicState {
    u32 blend_color = 0;
    u8 stencil_reference;
    u8 stencil_compare_mask;
    u8 stencil_write_mask;
};

union VertexBinding {
    u16 value = 0;
    BitField<0, 4, u16> binding;
    BitField<4, 1, u16> fixed;
    BitField<5, 11, u16> stride;
};

union VertexAttribute {
    u32 value = 0;
    BitField<0, 4, u32> binding;
    BitField<4, 4, u32> location;
    BitField<8, 3, Pica::PipelineRegs::VertexAttributeFormat> type;
    BitField<11, 3, u32> size;
    BitField<14, 11, u32> offset;
};

struct VertexLayout {
    u8 binding_count;
    u8 attribute_count;
    std::array<VertexBinding, MAX_VERTEX_BINDINGS> bindings;
    std::array<VertexAttribute, MAX_VERTEX_ATTRIBUTES> attributes;
};

/**
 * Information about a graphics/compute pipeline
 */
struct PipelineInfo {
    VertexLayout vertex_layout{};
    BlendingState blending{};
    VideoCore::PixelFormat color_attachment = VideoCore::PixelFormat::RGBA8;
    VideoCore::PixelFormat depth_attachment = VideoCore::PixelFormat::D24S8;
    RasterizationState rasterization{};
    DepthStencilState depth_stencil{};
    DynamicState dynamic;

    [[nodiscard]] bool IsDepthWriteEnabled() const noexcept {
        const bool has_stencil = depth_attachment == VideoCore::PixelFormat::D24S8;
        const bool depth_write =
            depth_stencil.depth_test_enable && depth_stencil.depth_write_enable;
        const bool stencil_write =
            has_stencil && depth_stencil.stencil_test_enable && dynamic.stencil_write_mask != 0;

        return depth_write || stencil_write;
    }
};

/**
 * Vulkan specialized PICA shader caches
 */
using ProgrammableVertexShaders = Pica::Shader::ShaderDoubleCache<PicaVSConfig, vk::ShaderModule,
                                                                  &Compile, &GenerateVertexShader>;

using FixedGeometryShaders = Pica::Shader::ShaderCache<PicaFixedGSConfig, vk::ShaderModule,
                                                       &Compile, &GenerateFixedGeometryShader>;

using FragmentShadersGLSL =
    Pica::Shader::ShaderCache<PicaFSConfig, vk::ShaderModule, &Compile, &GenerateFragmentShader>;

using FragmentShadersSPV = Pica::Shader::ShaderCache<PicaFSConfig, vk::ShaderModule, &CompileSPV,
                                                     &GenerateFragmentShaderSPV>;

class Instance;
class Scheduler;
class RenderpassCache;
class DescriptorManager;

/**
 * Stores a collection of rasterizer pipelines used during rendering.
 */
class PipelineCache {
public:
    PipelineCache(const Instance& instance, Scheduler& scheduler, RenderpassCache& renderpass_cache,
                  DescriptorManager& desc_manager);
    ~PipelineCache();

    /// Loads the pipeline cache stored to disk
    void LoadDiskCache();

    /// Stores the generated pipeline cache to disk
    void SaveDiskCache();

    /// Binds a pipeline using the provided information
    void BindPipeline(const PipelineInfo& info);

    /// Binds a PICA decompiled vertex shader
    bool UseProgrammableVertexShader(const Pica::Regs& regs, Pica::Shader::ShaderSetup& setup,
                                     const VertexLayout& layout);

    /// Binds a passthrough vertex shader
    void UseTrivialVertexShader();

    /// Binds a PICA decompiled geometry shader
    void UseFixedGeometryShader(const Pica::Regs& regs);

    /// Binds a passthrough geometry shader
    void UseTrivialGeometryShader();

    /// Binds a fragment shader generated from PICA state
    void UseFragmentShader(const Pica::Regs& regs);

    /// Binds a texture to the specified binding
    void BindTexture(u32 binding, vk::ImageView image_view);

    /// Binds a storage image to the specified binding
    void BindStorageImage(u32 binding, vk::ImageView image_view);

    /// Binds a buffer to the specified binding
    void BindBuffer(u32 binding, vk::Buffer buffer, u32 offset, u32 size);

    /// Binds a buffer to the specified binding
    void BindTexelBuffer(u32 binding, vk::BufferView buffer_view);

    /// Binds a sampler to the specified binding
    void BindSampler(u32 binding, vk::Sampler sampler);

    /// Sets the viewport rectangle to the provided values
    void SetViewport(float x, float y, float width, float height);

    /// Sets the scissor rectange to the provided values
    void SetScissor(s32 x, s32 y, u32 width, u32 height);

private:
    /// Applies dynamic pipeline state to the current command buffer
    void ApplyDynamic(const PipelineInfo& info);

    /// Builds the rasterizer pipeline layout
    void BuildLayout();

    /// Builds a rasterizer pipeline using the PipelineInfo struct
    vk::Pipeline BuildPipeline(const PipelineInfo& info);

    /// Returns true when the disk data can be used by the current driver
    bool IsCacheValid(const u8* data, u64 size) const;

    /// Create shader disk cache directories. Returns true on success.
    bool EnsureDirectories() const;

    /// Returns the pipeline cache storage dir
    std::string GetPipelineCacheDir() const;

private:
    const Instance& instance;
    Scheduler& scheduler;
    RenderpassCache& renderpass_cache;
    DescriptorManager& desc_manager;

    // Cached pipelines
    vk::PipelineCache pipeline_cache;
    std::unordered_map<u64, vk::Pipeline, Common::IdentityHash<u64>> graphics_pipelines;
    vk::Pipeline current_pipeline{};
    PipelineInfo current_info{};

    // Bound shader modules
    enum ProgramType : u32 { VS = 0, GS = 2, FS = 1 };

    std::array<vk::ShaderModule, MAX_SHADER_STAGES> current_shaders;
    std::array<u64, MAX_SHADER_STAGES> shader_hashes;
    ProgrammableVertexShaders programmable_vertex_shaders;
    FixedGeometryShaders fixed_geometry_shaders;
    FragmentShadersGLSL fragment_shaders_glsl;
    FragmentShadersSPV fragment_shaders_spv;
    vk::ShaderModule trivial_vertex_shader;
};

} // namespace Vulkan
