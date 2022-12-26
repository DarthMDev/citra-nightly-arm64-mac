// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/alignment.h"
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/microprofile.h"
#include "video_core/pica_state.h"
#include "video_core/regs_framebuffer.h"
#include "video_core/regs_pipeline.h"
#include "video_core/regs_rasterizer.h"
#include "video_core/renderer_vulkan/pica_to_vk.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/video_core.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

constexpr u32 VERTEX_BUFFER_SIZE = 64 * 1024 * 1024;
constexpr u32 INDEX_BUFFER_SIZE = 16 * 1024 * 1024;
constexpr u32 UNIFORM_BUFFER_SIZE = 16 * 1024 * 1024;
constexpr u32 TEXTURE_BUFFER_SIZE = 16 * 1024 * 1024;

constexpr std::array TEXTURE_BUFFER_LF_FORMATS = {vk::Format::eR32G32Sfloat};

constexpr std::array TEXTURE_BUFFER_FORMATS = {vk::Format::eR32G32Sfloat,
                                               vk::Format::eR32G32B32A32Sfloat};

constexpr VideoCore::SurfaceParams NULL_PARAMS = {.width = 1,
                                                  .height = 1,
                                                  .stride = 1,
                                                  .texture_type = VideoCore::TextureType::Texture2D,
                                                  .pixel_format = VideoCore::PixelFormat::RGBA8,
                                                  .type = VideoCore::SurfaceType::Color};

constexpr vk::ImageUsageFlags NULL_USAGE = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eTransferSrc |
                                           vk::ImageUsageFlagBits::eTransferDst;
constexpr vk::ImageUsageFlags NULL_STORAGE_USAGE = NULL_USAGE | vk::ImageUsageFlagBits::eStorage;

RasterizerVulkan::RasterizerVulkan(Frontend::EmuWindow& emu_window, const Instance& instance,
                                   Scheduler& scheduler, DescriptorManager& desc_manager,
                                   TextureRuntime& runtime, RenderpassCache& renderpass_cache)
    : instance{instance}, scheduler{scheduler}, runtime{runtime},
      renderpass_cache{renderpass_cache}, desc_manager{desc_manager}, res_cache{*this, runtime},
      pipeline_cache{instance, scheduler, renderpass_cache, desc_manager},
      null_surface{NULL_PARAMS, vk::Format::eR8G8B8A8Unorm, NULL_USAGE, runtime},
      null_storage_surface{NULL_PARAMS, vk::Format::eR32Uint, NULL_STORAGE_USAGE, runtime},
      vertex_buffer{
          instance, scheduler, VERTEX_BUFFER_SIZE, vk::BufferUsageFlagBits::eVertexBuffer, {}},
      uniform_buffer{
          instance, scheduler, UNIFORM_BUFFER_SIZE, vk::BufferUsageFlagBits::eUniformBuffer, {}},
      index_buffer{
          instance, scheduler, INDEX_BUFFER_SIZE, vk::BufferUsageFlagBits::eIndexBuffer, {}},
      texture_buffer{instance, scheduler, TEXTURE_BUFFER_SIZE,
                     vk::BufferUsageFlagBits::eUniformTexelBuffer, TEXTURE_BUFFER_FORMATS},
      texture_lf_buffer{instance, scheduler, TEXTURE_BUFFER_SIZE,
                        vk::BufferUsageFlagBits::eUniformTexelBuffer, TEXTURE_BUFFER_LF_FORMATS} {

    null_surface.Transition(vk::ImageLayout::eShaderReadOnlyOptimal, 0, 1);
    null_storage_surface.Transition(vk::ImageLayout::eGeneral, 0, 1);

    uniform_buffer_alignment = instance.UniformMinAlignment();
    uniform_size_aligned_vs =
        Common::AlignUp<std::size_t>(sizeof(Pica::Shader::VSUniformData), uniform_buffer_alignment);
    uniform_size_aligned_fs =
        Common::AlignUp<std::size_t>(sizeof(Pica::Shader::UniformData), uniform_buffer_alignment);

    // Define vertex layout for software shaders
    MakeSoftwareVertexLayout();
    pipeline_info.vertex_layout = software_layout;

    const SamplerInfo default_sampler_info = {
        .mag_filter = Pica::TexturingRegs::TextureConfig::TextureFilter::Linear,
        .min_filter = Pica::TexturingRegs::TextureConfig::TextureFilter::Linear,
        .mip_filter = Pica::TexturingRegs::TextureConfig::TextureFilter::Linear,
        .wrap_s = Pica::TexturingRegs::TextureConfig::WrapMode::ClampToBorder,
        .wrap_t = Pica::TexturingRegs::TextureConfig::WrapMode::ClampToBorder};

    default_sampler = CreateSampler(default_sampler_info);

    // Since we don't have access to VK_EXT_descriptor_indexing we need to intiallize
    // all descriptor sets even the ones we don't use. Use default_texture for this
    const u32 vs_uniform_size = sizeof(Pica::Shader::VSUniformData);
    const u32 fs_uniform_size = sizeof(Pica::Shader::UniformData);
    pipeline_cache.BindBuffer(0, uniform_buffer.GetHandle(), 0, vs_uniform_size);
    pipeline_cache.BindBuffer(1, uniform_buffer.GetHandle(), vs_uniform_size, fs_uniform_size);
    pipeline_cache.BindTexelBuffer(2, texture_lf_buffer.GetView());
    pipeline_cache.BindTexelBuffer(3, texture_buffer.GetView(0));
    pipeline_cache.BindTexelBuffer(4, texture_buffer.GetView(1));

    for (u32 i = 0; i < 4; i++) {
        pipeline_cache.BindTexture(i, null_surface.GetImageView());
        pipeline_cache.BindSampler(i, default_sampler);
    }

    for (u32 i = 0; i < 7; i++) {
        pipeline_cache.BindStorageImage(i, null_storage_surface.GetImageView());
    }

    // Explicitly call the derived version to avoid warnings about calling virtual
    // methods in the constructor
    RasterizerVulkan::SyncEntireState();
}

RasterizerVulkan::~RasterizerVulkan() {
    scheduler.Finish();

    vk::Device device = instance.GetDevice();

    for (auto& [key, sampler] : samplers) {
        device.destroySampler(sampler);
    }

    for (auto& [key, framebuffer] : framebuffers) {
        device.destroyFramebuffer(framebuffer);
    }

    device.destroySampler(default_sampler);
}

void RasterizerVulkan::LoadDiskResources(const std::atomic_bool& stop_loading,
                                         const VideoCore::DiskResourceLoadCallback& callback) {
    pipeline_cache.LoadDiskCache();
}

void RasterizerVulkan::SyncEntireState() {
    // Sync fixed function Vulkan state
    SyncFixedState();

    // Sync uniforms
    SyncClipCoef();
    SyncDepthScale();
    SyncDepthOffset();
    SyncAlphaTest();
    SyncCombinerColor();
    auto& tev_stages = Pica::g_state.regs.texturing.GetTevStages();
    for (std::size_t index = 0; index < tev_stages.size(); ++index)
        SyncTevConstColor(index, tev_stages[index]);

    SyncGlobalAmbient();
    for (unsigned light_index = 0; light_index < 8; light_index++) {
        SyncLightSpecular0(light_index);
        SyncLightSpecular1(light_index);
        SyncLightDiffuse(light_index);
        SyncLightAmbient(light_index);
        SyncLightPosition(light_index);
        SyncLightDistanceAttenuationBias(light_index);
        SyncLightDistanceAttenuationScale(light_index);
    }

    SyncFogColor();
    SyncProcTexNoise();
    SyncProcTexBias();
    SyncShadowBias();
    SyncShadowTextureBias();
}

void RasterizerVulkan::SyncFixedState() {
    SyncClipEnabled();
    SyncCullMode();
    SyncBlendEnabled();
    SyncBlendFuncs();
    SyncBlendColor();
    SyncLogicOp();
    SyncStencilTest();
    SyncDepthTest();
    SyncColorWriteMask();
    SyncStencilWriteMask();
    SyncDepthWriteMask();
}

void RasterizerVulkan::SetupVertexArray(u32 vs_input_size, u32 vs_input_index_min,
                                        u32 vs_input_index_max) {
    auto [array_ptr, array_offset, invalidate] = vertex_buffer.Map(vs_input_size);

    /**
     * The Nintendo 3DS has 12 attribute loaders which are used to tell the GPU
     * how to interpret vertex data. The program firsts sets GPUREG_ATTR_BUF_BASE to the base
     * address containing the vertex array data. The data for each attribute loader (i) can be found
     * by adding GPUREG_ATTR_BUFi_OFFSET to the base address. Attribute loaders can be thought
     * as something analogous to Vulkan bindings. The user can store attributes in separate loaders
     * or interleave them in the same loader.
     **/
    const auto& regs = Pica::g_state.regs;
    const auto& vertex_attributes = regs.pipeline.vertex_attributes;
    PAddr base_address = vertex_attributes.GetPhysicalBaseAddress(); // GPUREG_ATTR_BUF_BASE

    VertexLayout& layout = pipeline_info.vertex_layout;
    layout.attribute_count = 0;
    layout.binding_count = 0;
    enable_attributes.fill(false);

    u32 buffer_offset = 0;
    for (const auto& loader : vertex_attributes.attribute_loaders) {
        if (loader.component_count == 0 || loader.byte_count == 0) {
            continue;
        }

        // Analyze the attribute loader by checking which attributes it provides
        u32 offset = 0;
        for (u32 comp = 0; comp < loader.component_count && comp < 12; comp++) {
            u32 attribute_index = loader.GetComponent(comp);
            if (attribute_index < 12) {
                if (u32 size = vertex_attributes.GetNumElements(attribute_index); size != 0) {
                    offset = Common::AlignUp(
                        offset, vertex_attributes.GetElementSizeInBytes(attribute_index));

                    const u32 input_reg = regs.vs.GetRegisterForAttribute(attribute_index);
                    const Pica::PipelineRegs::VertexAttributeFormat format =
                        vertex_attributes.GetFormat(attribute_index);

                    VertexAttribute& attribute = layout.attributes[layout.attribute_count++];
                    attribute.binding.Assign(layout.binding_count);
                    attribute.location.Assign(input_reg);
                    attribute.offset.Assign(offset);
                    attribute.type.Assign(format);
                    attribute.size.Assign(size);

                    enable_attributes[input_reg] = true;
                    offset += vertex_attributes.GetStride(attribute_index);
                }
            } else {
                // Attribute ids 12, 13, 14 and 15 signify 4, 8, 12 and 16-byte paddings
                // respectively
                offset = Common::AlignUp(offset, 4);
                offset += (attribute_index - 11) * 4;
            }
        }

        const PAddr data_addr =
            base_address + loader.data_offset + (vs_input_index_min * loader.byte_count);
        const u32 vertex_num = vs_input_index_max - vs_input_index_min + 1;
        const u32 data_size = loader.byte_count * vertex_num;

        res_cache.FlushRegion(data_addr, data_size);
        std::memcpy(array_ptr + buffer_offset, VideoCore::g_memory->GetPhysicalPointer(data_addr),
                    data_size);

        // Create the binding associated with this loader
        VertexBinding& binding = layout.bindings[layout.binding_count];
        binding.binding.Assign(layout.binding_count);
        binding.fixed.Assign(0);
        binding.stride.Assign(loader.byte_count);

        // Keep track of the binding offsets so we can bind the vertex buffer later
        binding_offsets[layout.binding_count++] = array_offset + buffer_offset;
        buffer_offset += Common::AlignUp(data_size, 16);
    }

    binding_offsets[layout.binding_count] = array_offset + buffer_offset;
    vertex_buffer.Commit(buffer_offset);

    // Assign the rest of the attributes to the last binding
    SetupFixedAttribs();

    // Bind the generated bindings
    scheduler.Record([this, layout = pipeline_info.vertex_layout, offsets = binding_offsets](
                         vk::CommandBuffer render_cmdbuf, vk::CommandBuffer) {
        std::array<vk::Buffer, 16> buffers;
        buffers.fill(vertex_buffer.GetHandle());
        render_cmdbuf.bindVertexBuffers(0, layout.binding_count, buffers.data(), offsets.data());
    });
}

void RasterizerVulkan::SetupFixedAttribs() {
    const auto& regs = Pica::g_state.regs;
    const auto& vertex_attributes = regs.pipeline.vertex_attributes;
    VertexLayout& layout = pipeline_info.vertex_layout;

    auto [fixed_ptr, fixed_offset, _] = vertex_buffer.Map(16 * sizeof(Common::Vec4f));

    // Reserve the last binding for fixed and default attributes
    // Place the default attrib at offset zero for easy access
    static const Common::Vec4f default_attrib{0.f, 0.f, 0.f, 1.f};
    std::memcpy(fixed_ptr, default_attrib.AsArray(), sizeof(Common::Vec4f));

    // Find all fixed attributes and assign them to the last binding
    u32 offset = sizeof(Common::Vec4f);
    for (std::size_t i = 0; i < 16; i++) {
        if (vertex_attributes.IsDefaultAttribute(i)) {
            const u32 reg = regs.vs.GetRegisterForAttribute(i);
            if (!enable_attributes[reg]) {
                const auto& attr = Pica::g_state.input_default_attributes.attr[i];
                const std::array data = {attr.x.ToFloat32(), attr.y.ToFloat32(), attr.z.ToFloat32(),
                                         attr.w.ToFloat32()};

                const u32 data_size = sizeof(float) * static_cast<u32>(data.size());
                std::memcpy(fixed_ptr + offset, data.data(), data_size);

                VertexAttribute& attribute = layout.attributes[layout.attribute_count++];
                attribute.binding.Assign(layout.binding_count);
                attribute.location.Assign(reg);
                attribute.offset.Assign(offset);
                attribute.type.Assign(Pica::PipelineRegs::VertexAttributeFormat::FLOAT);
                attribute.size.Assign(4);

                offset += data_size;
                enable_attributes[reg] = true;
            }
        }
    }

    // Loop one more time to find unused attributes and assign them to the default one
    // If the attribute is just disabled, shove the default attribute to avoid
    // errors if the shader ever decides to use it.
    for (u32 i = 0; i < 16; i++) {
        if (!enable_attributes[i]) {
            VertexAttribute& attribute = layout.attributes[layout.attribute_count++];
            attribute.binding.Assign(layout.binding_count);
            attribute.location.Assign(i);
            attribute.offset.Assign(0);
            attribute.type.Assign(Pica::PipelineRegs::VertexAttributeFormat::FLOAT);
            attribute.size.Assign(4);
        }
    }

    // Define the fixed+default binding
    VertexBinding& binding = layout.bindings[layout.binding_count];
    binding.binding.Assign(layout.binding_count++);
    binding.fixed.Assign(1);
    binding.stride.Assign(offset);

    vertex_buffer.Commit(offset);
}

MICROPROFILE_DEFINE(Vulkan_VS, "Vulkan", "Vertex Shader Setup", MP_RGB(192, 128, 128));
bool RasterizerVulkan::SetupVertexShader() {
    MICROPROFILE_SCOPE(Vulkan_VS);
    return pipeline_cache.UseProgrammableVertexShader(Pica::g_state.regs, Pica::g_state.vs,
                                                      pipeline_info.vertex_layout);
}

MICROPROFILE_DEFINE(Vulkan_GS, "Vulkan", "Geometry Shader Setup", MP_RGB(128, 192, 128));
bool RasterizerVulkan::SetupGeometryShader() {
    MICROPROFILE_SCOPE(Vulkan_GS);
    const auto& regs = Pica::g_state.regs;

    if (regs.pipeline.use_gs != Pica::PipelineRegs::UseGS::No) {
        LOG_ERROR(Render_Vulkan, "Accelerate draw doesn't support geometry shader");
        return false;
    }

    pipeline_cache.UseFixedGeometryShader(regs);
    return true;
}

bool RasterizerVulkan::AccelerateDrawBatch(bool is_indexed) {
    const auto& regs = Pica::g_state.regs;
    if (regs.pipeline.use_gs != Pica::PipelineRegs::UseGS::No) {
        if (regs.pipeline.gs_config.mode != Pica::PipelineRegs::GSMode::Point) {
            return false;
        }
        if (regs.pipeline.triangle_topology != Pica::PipelineRegs::TriangleTopology::Shader) {
            return false;
        }
    }

    return Draw(true, is_indexed);
}

bool RasterizerVulkan::AccelerateDrawBatchInternal(bool is_indexed) {
    const auto& regs = Pica::g_state.regs;

    const auto [vs_input_index_min, vs_input_index_max, vs_input_size] =
        AnalyzeVertexArray(is_indexed);

    if (vs_input_size > VERTEX_BUFFER_SIZE) {
        LOG_WARNING(Render_Vulkan, "Too large vertex input size {}", vs_input_size);
        return false;
    }

    SetupVertexArray(vs_input_size, vs_input_index_min, vs_input_index_max);

    if (!SetupVertexShader()) {
        return false;
    }

    if (!SetupGeometryShader()) {
        return false;
    }

    pipeline_info.rasterization.topology.Assign(regs.pipeline.triangle_topology);
    pipeline_cache.BindPipeline(pipeline_info);

    if (is_indexed) {
        bool index_u16 = regs.pipeline.index_array.format != 0;
        const u32 index_buffer_size = regs.pipeline.num_vertices * (index_u16 ? 2 : 1);

        if (index_buffer_size > INDEX_BUFFER_SIZE) {
            LOG_WARNING(Render_Vulkan, "Too large index input size {}", index_buffer_size);
            return false;
        }

        const u8* index_data = VideoCore::g_memory->GetPhysicalPointer(
            regs.pipeline.vertex_attributes.GetPhysicalBaseAddress() +
            regs.pipeline.index_array.offset);

        // Upload index buffer data to the GPU
        auto [index_ptr, index_offset, _] = index_buffer.Map(index_buffer_size);
        std::memcpy(index_ptr, index_data, index_buffer_size);
        index_buffer.Commit(index_buffer_size);

        scheduler.Record([this, offset = index_offset, num_vertices = regs.pipeline.num_vertices,
                          index_u16, vertex_offset = vs_input_index_min](
                             vk::CommandBuffer render_cmdbuf, vk::CommandBuffer) {
            const vk::IndexType index_type =
                index_u16 ? vk::IndexType::eUint16 : vk::IndexType::eUint8EXT;
            render_cmdbuf.bindIndexBuffer(index_buffer.GetHandle(), offset, index_type);
            render_cmdbuf.drawIndexed(num_vertices, 1, 0, -vertex_offset, 0);
        });
    } else {
        scheduler.Record([num_vertices = regs.pipeline.num_vertices](
                             vk::CommandBuffer render_cmdbuf, vk::CommandBuffer) {
            render_cmdbuf.draw(num_vertices, 1, 0, 0);
        });
    }

    return true;
}

void RasterizerVulkan::DrawTriangles() {
    if (vertex_batch.empty()) {
        return;
    }

    Draw(false, false);
}

MICROPROFILE_DEFINE(Vulkan_Drawing, "Vulkan", "Drawing", MP_RGB(128, 128, 192));
bool RasterizerVulkan::Draw(bool accelerate, bool is_indexed) {
    MICROPROFILE_SCOPE(Vulkan_Drawing);
    const auto& regs = Pica::g_state.regs;

    const bool shadow_rendering = regs.framebuffer.IsShadowRendering();
    const bool has_stencil = regs.framebuffer.HasStencil();

    const bool write_color_fb = shadow_rendering || pipeline_info.blending.color_write_mask.Value();
    const bool write_depth_fb = pipeline_info.IsDepthWriteEnabled();
    const bool using_color_fb =
        regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress() != 0 && write_color_fb;
    const bool using_depth_fb =
        !shadow_rendering && regs.framebuffer.framebuffer.GetDepthBufferPhysicalAddress() != 0 &&
        (write_depth_fb || regs.framebuffer.output_merger.depth_test_enable != 0 ||
         (has_stencil && pipeline_info.depth_stencil.stencil_test_enable));

    const Common::Rectangle viewport_rect_unscaled = regs.rasterizer.GetViewportRect();

    auto [color_surface, depth_surface, surfaces_rect] =
        res_cache.GetFramebufferSurfaces(using_color_fb, using_depth_fb, viewport_rect_unscaled);

    if (!color_surface && shadow_rendering) {
        return true;
    }

    pipeline_info.color_attachment =
        color_surface ? color_surface->pixel_format : VideoCore::PixelFormat::Invalid;
    pipeline_info.depth_attachment =
        depth_surface ? depth_surface->pixel_format : VideoCore::PixelFormat::Invalid;

    const u16 res_scale = color_surface != nullptr
                              ? color_surface->res_scale
                              : (depth_surface == nullptr ? 1u : depth_surface->res_scale);

    const VideoCore::Rect2D draw_rect = {
        static_cast<u32>(std::clamp<s32>(static_cast<s32>(surfaces_rect.left) +
                                             viewport_rect_unscaled.left * res_scale,
                                         surfaces_rect.left, surfaces_rect.right)), // Left
        static_cast<u32>(std::clamp<s32>(static_cast<s32>(surfaces_rect.bottom) +
                                             viewport_rect_unscaled.top * res_scale,
                                         surfaces_rect.bottom, surfaces_rect.top)), // Top
        static_cast<u32>(std::clamp<s32>(static_cast<s32>(surfaces_rect.left) +
                                             viewport_rect_unscaled.right * res_scale,
                                         surfaces_rect.left, surfaces_rect.right)), // Right
        static_cast<u32>(std::clamp<s32>(static_cast<s32>(surfaces_rect.bottom) +
                                             viewport_rect_unscaled.bottom * res_scale,
                                         surfaces_rect.bottom, surfaces_rect.top))};

    if (uniform_block_data.data.framebuffer_scale != res_scale) {
        uniform_block_data.data.framebuffer_scale = res_scale;
        uniform_block_data.dirty = true;
    }

    // Scissor checks are window-, not viewport-relative, which means that if the cached texture
    // sub-rect changes, the scissor bounds also need to be updated.
    int scissor_x1 =
        static_cast<int>(surfaces_rect.left + regs.rasterizer.scissor_test.x1 * res_scale);
    int scissor_y1 =
        static_cast<int>(surfaces_rect.bottom + regs.rasterizer.scissor_test.y1 * res_scale);

    // x2, y2 have +1 added to cover the entire pixel area, otherwise you might get cracks when
    // scaling or doing multisampling.
    int scissor_x2 =
        static_cast<int>(surfaces_rect.left + (regs.rasterizer.scissor_test.x2 + 1) * res_scale);
    int scissor_y2 =
        static_cast<int>(surfaces_rect.bottom + (regs.rasterizer.scissor_test.y2 + 1) * res_scale);

    if (uniform_block_data.data.scissor_x1 != scissor_x1 ||
        uniform_block_data.data.scissor_x2 != scissor_x2 ||
        uniform_block_data.data.scissor_y1 != scissor_y1 ||
        uniform_block_data.data.scissor_y2 != scissor_y2) {

        uniform_block_data.data.scissor_x1 = scissor_x1;
        uniform_block_data.data.scissor_x2 = scissor_x2;
        uniform_block_data.data.scissor_y1 = scissor_y1;
        uniform_block_data.data.scissor_y2 = scissor_y2;
        uniform_block_data.dirty = true;
    }

    const auto BindCubeFace = [&](Pica::TexturingRegs::CubeFace face,
                                  Pica::Texture::TextureInfo& info) {
        info.physical_address = regs.texturing.GetCubePhysicalAddress(face);
        auto surface = res_cache.GetTextureSurface(info);

        const u32 binding = static_cast<u32>(face);
        if (surface) {
            pipeline_cache.BindStorageImage(binding, surface->GetImageView());
        } else {
            pipeline_cache.BindStorageImage(binding, null_storage_surface.GetImageView());
        }
    };

    const auto BindSampler = [&](u32 binding, SamplerInfo& info,
                                 const Pica::TexturingRegs::TextureConfig& config) {
        // TODO(GPUCode): Cubemaps don't contain any mipmaps for now, so sampling from them returns
        // nothing. Always sample from the base level until mipmaps for texture cubes are
        // implemented
        const bool skip_mipmap = config.type == Pica::TexturingRegs::TextureConfig::TextureCube;
        info = SamplerInfo{.mag_filter = config.mag_filter,
                           .min_filter = config.min_filter,
                           .mip_filter = config.mip_filter,
                           .wrap_s = config.wrap_s,
                           .wrap_t = config.wrap_t,
                           .border_color = config.border_color.raw,
                           .lod_min = skip_mipmap ? 0.f : static_cast<float>(config.lod.min_level),
                           .lod_max = skip_mipmap ? 0.f : static_cast<float>(config.lod.max_level),
                           .lod_bias = static_cast<float>(config.lod.bias)};

        // Search the cache and bind the appropriate sampler
        if (auto it = samplers.find(info); it != samplers.end()) {
            pipeline_cache.BindSampler(binding, it->second);
        } else {
            vk::Sampler texture_sampler = CreateSampler(info);
            samplers.emplace(info, texture_sampler);
            pipeline_cache.BindSampler(binding, texture_sampler);
        }
    };

    // Sync and bind the texture surfaces
    const auto pica_textures = regs.texturing.GetTextures();
    for (u32 texture_index = 0; texture_index < pica_textures.size(); ++texture_index) {
        const auto& texture = pica_textures[texture_index];

        if (texture.enabled) {
            if (texture_index == 0) {
                using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;
                switch (texture.config.type.Value()) {
                case TextureType::Shadow2D: {
                    auto surface = res_cache.GetTextureSurface(texture);
                    if (surface) {
                        surface->Transition(vk::ImageLayout::eGeneral, 0, surface->alloc.levels);
                        pipeline_cache.BindStorageImage(0, surface->GetStorageView());
                    } else {
                        pipeline_cache.BindStorageImage(0, null_storage_surface.GetImageView());
                    }
                    continue;
                }
                case TextureType::ShadowCube: {
                    using CubeFace = Pica::TexturingRegs::CubeFace;
                    auto info = Pica::Texture::TextureInfo::FromPicaRegister(texture.config,
                                                                             texture.format);
                    BindCubeFace(CubeFace::PositiveX, info);
                    BindCubeFace(CubeFace::NegativeX, info);
                    BindCubeFace(CubeFace::PositiveY, info);
                    BindCubeFace(CubeFace::NegativeY, info);
                    BindCubeFace(CubeFace::PositiveZ, info);
                    BindCubeFace(CubeFace::NegativeZ, info);
                    continue;
                }
                case TextureType::TextureCube: {
                    using CubeFace = Pica::TexturingRegs::CubeFace;
                    const VideoCore::TextureCubeConfig config = {
                        .px = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveX),
                        .nx = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeX),
                        .py = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveY),
                        .ny = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeY),
                        .pz = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveZ),
                        .nz = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeZ),
                        .width = texture.config.width,
                        .format = texture.format};

                    auto surface = res_cache.GetTextureCube(config);
                    if (surface) {
                        surface->Transition(vk::ImageLayout::eShaderReadOnlyOptimal, 0,
                                            surface->alloc.levels);
                        pipeline_cache.BindTexture(3, surface->GetImageView());
                    } else {
                        pipeline_cache.BindTexture(3, null_surface.GetImageView());
                    }

                    BindSampler(3, texture_cube_sampler, texture.config);
                    continue; // Texture unit 0 setup finished. Continue to next unit
                }
                default:
                    break;
                }
            }

            // Update sampler key
            BindSampler(texture_index, texture_samplers[texture_index], texture.config);

            auto surface = res_cache.GetTextureSurface(texture);
            if (surface) {
                if (color_surface && color_surface->GetImageView() == surface->GetImageView()) {
                    Surface temp{*color_surface, runtime};
                    const VideoCore::TextureCopy copy = {
                        .src_level = 0,
                        .dst_level = 0,
                        .src_layer = 0,
                        .dst_layer = 0,
                        .src_offset = VideoCore::Offset{0, 0},
                        .dst_offset = VideoCore::Offset{0, 0},
                        .extent = VideoCore::Extent{temp.GetScaledWidth(), temp.GetScaledHeight()}};

                    runtime.CopyTextures(*color_surface, temp, copy);
                    temp.Transition(vk::ImageLayout::eShaderReadOnlyOptimal, 0, temp.alloc.levels);

                    pipeline_cache.BindTexture(texture_index, temp.GetImageView());
                } else {
                    surface->Transition(vk::ImageLayout::eShaderReadOnlyOptimal, 0,
                                        surface->alloc.levels);
                    pipeline_cache.BindTexture(texture_index, surface->GetImageView());
                }

            } else {
                // Can occur when texture addr is null or its memory is unmapped/invalid
                // HACK: In this case, the correct behaviour for the PICA is to use the last
                // rendered colour. But because this would be impractical to implement, the
                // next best alternative is to use a clear texture, essentially skipping
                // the geometry in question.
                // For example: a bug in Pokemon X/Y causes NULL-texture squares to be drawn
                // on the male character's face, which in the OpenGL default appear black.
                pipeline_cache.BindTexture(texture_index, null_surface.GetImageView());
            }
        } else {
            pipeline_cache.BindTexture(texture_index, null_surface.GetImageView());
            pipeline_cache.BindSampler(texture_index, default_sampler);
        }
    }

    // NOTE: From here onwards its a safe zone to set the draw state, doing that any earlier will
    // cause issues as the rasterizer cache might cause a scheduler switch and invalidate our state

    // Sync the viewport
    pipeline_cache.SetViewport(surfaces_rect.left + viewport_rect_unscaled.left * res_scale,
                               surfaces_rect.bottom + viewport_rect_unscaled.bottom * res_scale,
                               viewport_rect_unscaled.GetWidth() * res_scale,
                               viewport_rect_unscaled.GetHeight() * res_scale);

    // Sync and bind the shader
    if (shader_dirty) {
        pipeline_cache.UseFragmentShader(regs);
        shader_dirty = false;
    }

    // Sync the LUTs within the texture buffer
    SyncAndUploadLUTs();
    SyncAndUploadLUTsLF();

    // Sync the uniform data
    UploadUniforms(accelerate);

    // Viewport can have negative offsets or larger dimensions than our framebuffer sub-rect.
    // Enable scissor test to prevent drawing outside of the framebuffer region
    pipeline_cache.SetScissor(draw_rect.left, draw_rect.bottom, draw_rect.GetWidth(),
                              draw_rect.GetHeight());

    const FramebufferInfo framebuffer_info = {
        .color = color_surface ? color_surface->GetFramebufferView() : VK_NULL_HANDLE,
        .depth = depth_surface ? depth_surface->GetFramebufferView() : VK_NULL_HANDLE,
        .renderpass = renderpass_cache.GetRenderpass(pipeline_info.color_attachment,
                                                     pipeline_info.depth_attachment, false),
        .width = surfaces_rect.GetWidth(),
        .height = surfaces_rect.GetHeight()};

    auto [it, new_framebuffer] = framebuffers.try_emplace(framebuffer_info, vk::Framebuffer{});
    if (new_framebuffer) {
        it->second = CreateFramebuffer(framebuffer_info);
    }

    if (color_surface) {
        color_surface->Transition(vk::ImageLayout::eColorAttachmentOptimal, 0, 1);
    }

    if (depth_surface) {
        depth_surface->Transition(vk::ImageLayout::eDepthStencilAttachmentOptimal, 0, 1);
    }

    const RenderpassState renderpass_info = {
        .renderpass = framebuffer_info.renderpass,
        .framebuffer = it->second,
        .render_area = vk::Rect2D{.offset = {static_cast<s32>(draw_rect.left),
                                             static_cast<s32>(draw_rect.bottom)},
                                  .extent = {draw_rect.GetWidth(), draw_rect.GetHeight()}},
        .clear = {}};

    renderpass_cache.EnterRenderpass(renderpass_info);

    // Draw the vertex batch
    bool succeeded = true;
    if (accelerate) {
        succeeded = AccelerateDrawBatchInternal(is_indexed);
    } else {
        pipeline_info.rasterization.topology.Assign(Pica::PipelineRegs::TriangleTopology::List);
        pipeline_info.vertex_layout = software_layout;
        pipeline_cache.UseTrivialVertexShader();
        pipeline_cache.UseTrivialGeometryShader();
        pipeline_cache.BindPipeline(pipeline_info);

        const u32 max_vertices = VERTEX_BUFFER_SIZE / sizeof(HardwareVertex);
        const u32 batch_size = static_cast<u32>(vertex_batch.size());
        for (u32 base_vertex = 0; base_vertex < batch_size; base_vertex += max_vertices) {
            const u32 vertices = std::min(max_vertices, batch_size - base_vertex);
            const u32 vertex_size = vertices * sizeof(HardwareVertex);

            // Copy vertex data
            auto [array_ptr, offset, _] = vertex_buffer.Map(vertex_size);
            std::memcpy(array_ptr, vertex_batch.data() + base_vertex, vertex_size);
            vertex_buffer.Commit(vertex_size);

            scheduler.Record([this, vertices, base_vertex,
                              offset = offset](vk::CommandBuffer render_cmdbuf, vk::CommandBuffer) {
                render_cmdbuf.bindVertexBuffers(0, vertex_buffer.GetHandle(), offset);
                render_cmdbuf.draw(vertices, 1, base_vertex, 0);
            });
        }
    }

    vertex_batch.clear();

    // Mark framebuffer surfaces as dirty
    const Common::Rectangle draw_rect_unscaled{draw_rect / res_scale};
    if (color_surface && write_color_fb) {
        auto interval = color_surface->GetSubRectInterval(draw_rect_unscaled);
        res_cache.InvalidateRegion(boost::icl::first(interval), boost::icl::length(interval),
                                   color_surface);
    }

    if (depth_surface && write_depth_fb) {
        auto interval = depth_surface->GetSubRectInterval(draw_rect_unscaled);
        res_cache.InvalidateRegion(boost::icl::first(interval), boost::icl::length(interval),
                                   depth_surface);
    }

    return succeeded;
}

void RasterizerVulkan::NotifyFixedFunctionPicaRegisterChanged(u32 id) {
    switch (id) {
    // Culling
    case PICA_REG_INDEX(rasterizer.cull_mode):
        SyncCullMode();
        break;

    // Clipping plane
    case PICA_REG_INDEX(rasterizer.clip_enable):
        SyncClipEnabled();
        break;

    case PICA_REG_INDEX(rasterizer.clip_coef[0]):
    case PICA_REG_INDEX(rasterizer.clip_coef[1]):
    case PICA_REG_INDEX(rasterizer.clip_coef[2]):
    case PICA_REG_INDEX(rasterizer.clip_coef[3]):
        SyncClipCoef();
        break;

    // Blending
    case PICA_REG_INDEX(framebuffer.output_merger.alphablend_enable):
        if (instance.NeedsLogicOpEmulation()) {
            // We need this in the fragment shader to emulate logic operations
            shader_dirty = true;
        }
        SyncBlendEnabled();
        break;
    case PICA_REG_INDEX(framebuffer.output_merger.alpha_blending):
        SyncBlendFuncs();
        break;
    case PICA_REG_INDEX(framebuffer.output_merger.blend_const):
        SyncBlendColor();
        break;

    // Sync VK stencil test + stencil write mask
    // (Pica stencil test function register also contains a stencil write mask)
    case PICA_REG_INDEX(framebuffer.output_merger.stencil_test.raw_func):
        SyncStencilTest();
        SyncStencilWriteMask();
        break;
    case PICA_REG_INDEX(framebuffer.output_merger.stencil_test.raw_op):
    case PICA_REG_INDEX(framebuffer.framebuffer.depth_format):
        SyncStencilTest();
        break;

    // Sync VK depth test + depth and color write mask
    // (Pica depth test function register also contains a depth and color write mask)
    case PICA_REG_INDEX(framebuffer.output_merger.depth_test_enable):
        SyncDepthTest();
        SyncDepthWriteMask();
        SyncColorWriteMask();
        break;

    // Sync VK depth and stencil write mask
    // (This is a dedicated combined depth / stencil write-enable register)
    case PICA_REG_INDEX(framebuffer.framebuffer.allow_depth_stencil_write):
        SyncDepthWriteMask();
        SyncStencilWriteMask();
        break;

    // Sync VK color write mask
    // (This is a dedicated color write-enable register)
    case PICA_REG_INDEX(framebuffer.framebuffer.allow_color_write):
        SyncColorWriteMask();
        break;

    // Logic op
    case PICA_REG_INDEX(framebuffer.output_merger.logic_op):
        if (instance.NeedsLogicOpEmulation()) {
            // We need this in the fragment shader to emulate logic operations
            shader_dirty = true;
        }
        SyncLogicOp();
        break;
    }
}

MICROPROFILE_DEFINE(Vulkan_CacheManagement, "Vulkan", "Cache Mgmt", MP_RGB(100, 255, 100));
void RasterizerVulkan::FlushAll() {
    MICROPROFILE_SCOPE(Vulkan_CacheManagement);
    res_cache.FlushAll();
}

void RasterizerVulkan::FlushRegion(PAddr addr, u32 size) {
    MICROPROFILE_SCOPE(Vulkan_CacheManagement);
    res_cache.FlushRegion(addr, size);
}

void RasterizerVulkan::InvalidateRegion(PAddr addr, u32 size) {
    MICROPROFILE_SCOPE(Vulkan_CacheManagement);
    res_cache.InvalidateRegion(addr, size, nullptr);
}

void RasterizerVulkan::FlushAndInvalidateRegion(PAddr addr, u32 size) {
    MICROPROFILE_SCOPE(Vulkan_CacheManagement);
    res_cache.FlushRegion(addr, size);
    res_cache.InvalidateRegion(addr, size, nullptr);
}

MICROPROFILE_DEFINE(Vulkan_Blits, "Vulkan", "Blits", MP_RGB(100, 100, 255));
bool RasterizerVulkan::AccelerateDisplayTransfer(const GPU::Regs::DisplayTransferConfig& config) {
    MICROPROFILE_SCOPE(Vulkan_Blits);

    VideoCore::SurfaceParams src_params;
    src_params.addr = config.GetPhysicalInputAddress();
    src_params.width = config.output_width;
    src_params.stride = config.input_width;
    src_params.height = config.output_height;
    src_params.is_tiled = !config.input_linear;
    src_params.pixel_format = VideoCore::PixelFormatFromGPUPixelFormat(config.input_format);
    src_params.UpdateParams();

    VideoCore::SurfaceParams dst_params;
    dst_params.addr = config.GetPhysicalOutputAddress();
    dst_params.width = config.scaling != config.NoScale ? config.output_width.Value() / 2
                                                        : config.output_width.Value();
    dst_params.height = config.scaling == config.ScaleXY ? config.output_height.Value() / 2
                                                         : config.output_height.Value();
    dst_params.is_tiled = config.input_linear != config.dont_swizzle;
    dst_params.pixel_format = VideoCore::PixelFormatFromGPUPixelFormat(config.output_format);
    dst_params.UpdateParams();

    auto [src_surface, src_rect] =
        res_cache.GetSurfaceSubRect(src_params, VideoCore::ScaleMatch::Ignore, true);
    if (src_surface == nullptr)
        return false;

    dst_params.res_scale = src_surface->res_scale;

    auto [dst_surface, dst_rect] =
        res_cache.GetSurfaceSubRect(dst_params, VideoCore::ScaleMatch::Upscale, false);
    if (dst_surface == nullptr) {
        return false;
    }

    if (src_surface->is_tiled != dst_surface->is_tiled)
        std::swap(src_rect.top, src_rect.bottom);

    if (config.flip_vertically)
        std::swap(src_rect.top, src_rect.bottom);

    if (!res_cache.BlitSurfaces(src_surface, src_rect, dst_surface, dst_rect))
        return false;

    res_cache.InvalidateRegion(dst_params.addr, dst_params.size, dst_surface);
    return true;
}

bool RasterizerVulkan::AccelerateTextureCopy(const GPU::Regs::DisplayTransferConfig& config) {
    u32 copy_size = Common::AlignDown(config.texture_copy.size, 16);
    if (copy_size == 0) {
        return false;
    }

    u32 input_gap = config.texture_copy.input_gap * 16;
    u32 input_width = config.texture_copy.input_width * 16;
    if (input_width == 0 && input_gap != 0) {
        return false;
    }
    if (input_gap == 0 || input_width >= copy_size) {
        input_width = copy_size;
        input_gap = 0;
    }
    if (copy_size % input_width != 0) {
        return false;
    }

    u32 output_gap = config.texture_copy.output_gap * 16;
    u32 output_width = config.texture_copy.output_width * 16;
    if (output_width == 0 && output_gap != 0) {
        return false;
    }
    if (output_gap == 0 || output_width >= copy_size) {
        output_width = copy_size;
        output_gap = 0;
    }
    if (copy_size % output_width != 0) {
        return false;
    }

    VideoCore::SurfaceParams src_params;
    src_params.addr = config.GetPhysicalInputAddress();
    src_params.stride = input_width + input_gap; // stride in bytes
    src_params.width = input_width;              // width in bytes
    src_params.height = copy_size / input_width;
    src_params.size = ((src_params.height - 1) * src_params.stride) + src_params.width;
    src_params.end = src_params.addr + src_params.size;

    auto [src_surface, src_rect] = res_cache.GetTexCopySurface(src_params);
    if (src_surface == nullptr) {
        return false;
    }

    if (output_gap != 0 &&
        (output_width != src_surface->BytesInPixels(src_rect.GetWidth() / src_surface->res_scale) *
                             (src_surface->is_tiled ? 8 : 1) ||
         output_gap % src_surface->BytesInPixels(src_surface->is_tiled ? 64 : 1) != 0)) {
        return false;
    }

    VideoCore::SurfaceParams dst_params = *src_surface;
    dst_params.addr = config.GetPhysicalOutputAddress();
    dst_params.width = src_rect.GetWidth() / src_surface->res_scale;
    dst_params.stride = dst_params.width + src_surface->PixelsInBytes(
                                               src_surface->is_tiled ? output_gap / 8 : output_gap);
    dst_params.height = src_rect.GetHeight() / src_surface->res_scale;
    dst_params.res_scale = src_surface->res_scale;
    dst_params.UpdateParams();

    // Since we are going to invalidate the gap if there is one, we will have to load it first
    const bool load_gap = output_gap != 0;
    auto [dst_surface, dst_rect] =
        res_cache.GetSurfaceSubRect(dst_params, VideoCore::ScaleMatch::Upscale, load_gap);

    if (!dst_surface || dst_surface->type == VideoCore::SurfaceType::Texture ||
        !res_cache.BlitSurfaces(src_surface, src_rect, dst_surface, dst_rect)) {
        return false;
    }

    res_cache.InvalidateRegion(dst_params.addr, dst_params.size, dst_surface);
    return true;
}

bool RasterizerVulkan::AccelerateFill(const GPU::Regs::MemoryFillConfig& config) {
    auto dst_surface = res_cache.GetFillSurface(config);
    if (dst_surface == nullptr)
        return false;

    res_cache.InvalidateRegion(dst_surface->addr, dst_surface->size, dst_surface);
    return true;
}

bool RasterizerVulkan::AccelerateDisplay(const GPU::Regs::FramebufferConfig& config,
                                         PAddr framebuffer_addr, u32 pixel_stride,
                                         ScreenInfo& screen_info) {
    if (framebuffer_addr == 0) {
        return false;
    }
    MICROPROFILE_SCOPE(Vulkan_CacheManagement);

    VideoCore::SurfaceParams src_params;
    src_params.addr = framebuffer_addr;
    src_params.width = std::min(config.width.Value(), pixel_stride);
    src_params.height = config.height;
    src_params.stride = pixel_stride;
    src_params.is_tiled = false;
    src_params.pixel_format = VideoCore::PixelFormatFromGPUPixelFormat(config.color_format);
    src_params.UpdateParams();

    const auto [src_surface, src_rect] =
        res_cache.GetSurfaceSubRect(src_params, VideoCore::ScaleMatch::Ignore, true);

    if (src_surface == nullptr) {
        return false;
    }

    u32 scaled_width = src_surface->GetScaledWidth();
    u32 scaled_height = src_surface->GetScaledHeight();

    screen_info.display_texcoords = Common::Rectangle<float>(
        (float)src_rect.bottom / (float)scaled_height, (float)src_rect.left / (float)scaled_width,
        (float)src_rect.top / (float)scaled_height, (float)src_rect.right / (float)scaled_width);

    screen_info.display_texture = &src_surface->alloc;

    return true;
}

void RasterizerVulkan::MakeSoftwareVertexLayout() {
    constexpr std::array sizes = {4, 4, 2, 2, 2, 1, 4, 3};

    software_layout = VertexLayout{.binding_count = 1, .attribute_count = 8};

    for (u32 i = 0; i < software_layout.binding_count; i++) {
        VertexBinding& binding = software_layout.bindings[i];
        binding.binding.Assign(i);
        binding.fixed.Assign(0);
        binding.stride.Assign(sizeof(HardwareVertex));
    }

    u32 offset = 0;
    for (u32 i = 0; i < 8; i++) {
        VertexAttribute& attribute = software_layout.attributes[i];
        attribute.binding.Assign(0);
        attribute.location.Assign(i);
        attribute.offset.Assign(offset);
        attribute.type.Assign(Pica::PipelineRegs::VertexAttributeFormat::FLOAT);
        attribute.size.Assign(sizes[i]);
        offset += sizes[i] * sizeof(float);
    }
}

vk::Sampler RasterizerVulkan::CreateSampler(const SamplerInfo& info) {
    const bool use_border_color = instance.IsCustomBorderColorSupported() &&
                                  (info.wrap_s == SamplerInfo::TextureConfig::ClampToBorder ||
                                   info.wrap_t == SamplerInfo::TextureConfig::ClampToBorder);
    auto properties = instance.GetPhysicalDevice().getProperties();

    const auto color = PicaToVK::ColorRGBA8(info.border_color);
    const vk::SamplerCustomBorderColorCreateInfoEXT border_color_info = {
        .customBorderColor =
            vk::ClearColorValue{.float32 = std::array{color[0], color[1], color[2], color[3]}},
        .format = vk::Format::eUndefined};

    const vk::SamplerCreateInfo sampler_info = {
        .pNext = use_border_color ? &border_color_info : nullptr,
        .magFilter = PicaToVK::TextureFilterMode(info.mag_filter),
        .minFilter = PicaToVK::TextureFilterMode(info.min_filter),
        .mipmapMode = PicaToVK::TextureMipFilterMode(info.mip_filter),
        .addressModeU = PicaToVK::WrapMode(info.wrap_s),
        .addressModeV = PicaToVK::WrapMode(info.wrap_t),
        .mipLodBias = info.lod_bias / 256.0f,
        .anisotropyEnable = true,
        .maxAnisotropy = properties.limits.maxSamplerAnisotropy,
        .compareEnable = false,
        .compareOp = vk::CompareOp::eAlways,
        .minLod = info.lod_min,
        .maxLod = info.lod_max,
        .borderColor =
            use_border_color ? vk::BorderColor::eFloatCustomEXT : vk::BorderColor::eIntOpaqueBlack,
        .unnormalizedCoordinates = false};

    vk::Device device = instance.GetDevice();
    return device.createSampler(sampler_info);
}

vk::Framebuffer RasterizerVulkan::CreateFramebuffer(const FramebufferInfo& info) {
    u32 attachment_count = 0;
    std::array<vk::ImageView, 2> attachments;

    if (info.color) {
        attachments[attachment_count++] = info.color;
    }

    if (info.depth) {
        attachments[attachment_count++] = info.depth;
    }

    const vk::FramebufferCreateInfo framebuffer_info = {.renderPass = info.renderpass,
                                                        .attachmentCount = attachment_count,
                                                        .pAttachments = attachments.data(),
                                                        .width = info.width,
                                                        .height = info.height,
                                                        .layers = 1};

    vk::Device device = instance.GetDevice();
    return device.createFramebuffer(framebuffer_info);
}

void RasterizerVulkan::FlushBuffers() {
    vertex_buffer.Flush();
    uniform_buffer.Flush();
    index_buffer.Flush();
    texture_buffer.Flush();
    texture_lf_buffer.Flush();
}

void RasterizerVulkan::SyncClipEnabled() {
    uniform_block_data.data.enable_clip1 = Pica::g_state.regs.rasterizer.clip_enable != 0;
}

void RasterizerVulkan::SyncClipCoef() {
    const auto raw_clip_coef = Pica::g_state.regs.rasterizer.GetClipCoef();
    const Common::Vec4f new_clip_coef = {raw_clip_coef.x.ToFloat32(), raw_clip_coef.y.ToFloat32(),
                                         raw_clip_coef.z.ToFloat32(), raw_clip_coef.w.ToFloat32()};
    if (new_clip_coef != uniform_block_data.data.clip_coef) {
        uniform_block_data.data.clip_coef = new_clip_coef;
        uniform_block_data.dirty = true;
    }
}

void RasterizerVulkan::SyncCullMode() {
    const auto& regs = Pica::g_state.regs;
    pipeline_info.rasterization.cull_mode.Assign(regs.rasterizer.cull_mode);
}

void RasterizerVulkan::SyncBlendEnabled() {
    pipeline_info.blending.blend_enable.Assign(
        Pica::g_state.regs.framebuffer.output_merger.alphablend_enable);
}

void RasterizerVulkan::SyncBlendFuncs() {
    const auto& regs = Pica::g_state.regs;

    pipeline_info.blending.color_blend_eq.Assign(
        regs.framebuffer.output_merger.alpha_blending.blend_equation_rgb);
    pipeline_info.blending.alpha_blend_eq.Assign(
        regs.framebuffer.output_merger.alpha_blending.blend_equation_a);
    pipeline_info.blending.src_color_blend_factor.Assign(
        regs.framebuffer.output_merger.alpha_blending.factor_source_rgb);
    pipeline_info.blending.dst_color_blend_factor.Assign(
        regs.framebuffer.output_merger.alpha_blending.factor_dest_rgb);
    pipeline_info.blending.src_alpha_blend_factor.Assign(
        regs.framebuffer.output_merger.alpha_blending.factor_source_a);
    pipeline_info.blending.dst_alpha_blend_factor.Assign(
        regs.framebuffer.output_merger.alpha_blending.factor_dest_a);
}

void RasterizerVulkan::SyncBlendColor() {
    const auto& regs = Pica::g_state.regs;
    pipeline_info.dynamic.blend_color = regs.framebuffer.output_merger.blend_const.raw;
}

void RasterizerVulkan::SyncLogicOp() {
    const auto& regs = Pica::g_state.regs;

    const bool is_logic_op_emulated =
        instance.NeedsLogicOpEmulation() && !regs.framebuffer.output_merger.alphablend_enable;
    const bool is_logic_op_noop =
        regs.framebuffer.output_merger.logic_op == Pica::FramebufferRegs::LogicOp::NoOp;
    if (is_logic_op_emulated && is_logic_op_noop) {
        // Color output is disabled by logic operation. We use color write mask to skip
        // color but allow depth write.
        pipeline_info.blending.color_write_mask.Assign(0);
    } else {
        pipeline_info.blending.logic_op.Assign(regs.framebuffer.output_merger.logic_op);
    }
}

void RasterizerVulkan::SyncColorWriteMask() {
    const auto& regs = Pica::g_state.regs;
    const u32 color_mask = (regs.framebuffer.output_merger.depth_color_mask >> 8) & 0xF;

    const bool is_logic_op_emulated =
        instance.NeedsLogicOpEmulation() && !regs.framebuffer.output_merger.alphablend_enable;
    const bool is_logic_op_noop =
        regs.framebuffer.output_merger.logic_op == Pica::FramebufferRegs::LogicOp::NoOp;
    if (is_logic_op_emulated && is_logic_op_noop) {
        // Color output is disabled by logic operation. We use color write mask to skip
        // color but allow depth write. Return early to avoid overwriting this.
        return;
    }

    pipeline_info.blending.color_write_mask.Assign(color_mask);
}

void RasterizerVulkan::SyncStencilWriteMask() {
    const auto& regs = Pica::g_state.regs;
    pipeline_info.dynamic.stencil_write_mask =
        (regs.framebuffer.framebuffer.allow_depth_stencil_write != 0)
            ? static_cast<u32>(regs.framebuffer.output_merger.stencil_test.write_mask)
            : 0;
}

void RasterizerVulkan::SyncDepthWriteMask() {
    const auto& regs = Pica::g_state.regs;

    const bool write_enable = (regs.framebuffer.framebuffer.allow_depth_stencil_write != 0 &&
                               regs.framebuffer.output_merger.depth_write_enable);
    pipeline_info.depth_stencil.depth_write_enable.Assign(write_enable);
}

void RasterizerVulkan::SyncStencilTest() {
    const auto& regs = Pica::g_state.regs;

    const auto& stencil_test = regs.framebuffer.output_merger.stencil_test;
    const bool test_enable = stencil_test.enable && regs.framebuffer.framebuffer.depth_format ==
                                                        Pica::FramebufferRegs::DepthFormat::D24S8;

    pipeline_info.depth_stencil.stencil_test_enable.Assign(test_enable);
    pipeline_info.depth_stencil.stencil_fail_op.Assign(stencil_test.action_stencil_fail);
    pipeline_info.depth_stencil.stencil_pass_op.Assign(stencil_test.action_depth_pass);
    pipeline_info.depth_stencil.stencil_depth_fail_op.Assign(stencil_test.action_depth_fail);
    pipeline_info.depth_stencil.stencil_compare_op.Assign(stencil_test.func);
    pipeline_info.dynamic.stencil_reference = stencil_test.reference_value;
    pipeline_info.dynamic.stencil_compare_mask = stencil_test.input_mask;
}

void RasterizerVulkan::SyncDepthTest() {
    const auto& regs = Pica::g_state.regs;

    const bool test_enabled = regs.framebuffer.output_merger.depth_test_enable == 1 ||
                              regs.framebuffer.output_merger.depth_write_enable == 1;
    const auto compare_op = regs.framebuffer.output_merger.depth_test_enable == 1
                                ? regs.framebuffer.output_merger.depth_test_func.Value()
                                : Pica::FramebufferRegs::CompareFunc::Always;

    pipeline_info.depth_stencil.depth_test_enable.Assign(test_enabled);
    pipeline_info.depth_stencil.depth_compare_op.Assign(compare_op);
}

void RasterizerVulkan::SyncAndUploadLUTsLF() {
    constexpr std::size_t max_size =
        sizeof(Common::Vec2f) * 256 * Pica::LightingRegs::NumLightingSampler +
        sizeof(Common::Vec2f) * 128; // fog

    if (!uniform_block_data.lighting_lut_dirty_any && !uniform_block_data.fog_lut_dirty) {
        return;
    }

    std::size_t bytes_used = 0;
    auto [buffer, offset, invalidate] = texture_lf_buffer.Map(max_size);

    // Sync the lighting luts
    if (uniform_block_data.lighting_lut_dirty_any || invalidate) {
        for (unsigned index = 0; index < uniform_block_data.lighting_lut_dirty.size(); index++) {
            if (uniform_block_data.lighting_lut_dirty[index] || invalidate) {
                std::array<Common::Vec2f, 256> new_data;
                const auto& source_lut = Pica::g_state.lighting.luts[index];
                std::transform(source_lut.begin(), source_lut.end(), new_data.begin(),
                               [](const auto& entry) {
                                   return Common::Vec2f{entry.ToFloat(), entry.DiffToFloat()};
                               });

                if (new_data != lighting_lut_data[index] || invalidate) {
                    lighting_lut_data[index] = new_data;
                    std::memcpy(buffer + bytes_used, new_data.data(),
                                new_data.size() * sizeof(Common::Vec2f));
                    uniform_block_data.data.lighting_lut_offset[index / 4][index % 4] =
                        static_cast<int>((offset + bytes_used) / sizeof(Common::Vec2f));
                    uniform_block_data.dirty = true;
                    bytes_used += new_data.size() * sizeof(Common::Vec2f);
                }
                uniform_block_data.lighting_lut_dirty[index] = false;
            }
        }
        uniform_block_data.lighting_lut_dirty_any = false;
    }

    // Sync the fog lut
    if (uniform_block_data.fog_lut_dirty || invalidate) {
        std::array<Common::Vec2f, 128> new_data;

        std::transform(Pica::g_state.fog.lut.begin(), Pica::g_state.fog.lut.end(), new_data.begin(),
                       [](const auto& entry) {
                           return Common::Vec2f{entry.ToFloat(), entry.DiffToFloat()};
                       });

        if (new_data != fog_lut_data || invalidate) {
            fog_lut_data = new_data;
            std::memcpy(buffer + bytes_used, new_data.data(),
                        new_data.size() * sizeof(Common::Vec2f));
            uniform_block_data.data.fog_lut_offset =
                static_cast<int>((offset + bytes_used) / sizeof(Common::Vec2f));
            uniform_block_data.dirty = true;
            bytes_used += new_data.size() * sizeof(Common::Vec2f);
        }
        uniform_block_data.fog_lut_dirty = false;
    }

    texture_lf_buffer.Commit(static_cast<u32>(bytes_used));
}

void RasterizerVulkan::SyncAndUploadLUTs() {
    constexpr std::size_t max_size =
        sizeof(Common::Vec2f) * 128 * 3 + // proctex: noise + color + alpha
        sizeof(Common::Vec4f) * 256 +     // proctex
        sizeof(Common::Vec4f) * 256;      // proctex diff

    if (!uniform_block_data.proctex_noise_lut_dirty &&
        !uniform_block_data.proctex_color_map_dirty &&
        !uniform_block_data.proctex_alpha_map_dirty && !uniform_block_data.proctex_lut_dirty &&
        !uniform_block_data.proctex_diff_lut_dirty) {
        return;
    }

    std::size_t bytes_used = 0;
    auto [buffer, offset, invalidate] = texture_buffer.Map(max_size);

    // helper function for SyncProcTexNoiseLUT/ColorMap/AlphaMap
    auto SyncProcTexValueLUT =
        [this, &buffer = buffer, &offset = offset, &invalidate = invalidate,
         &bytes_used](const std::array<Pica::State::ProcTex::ValueEntry, 128>& lut,
                      std::array<Common::Vec2f, 128>& lut_data, int& lut_offset) {
            std::array<Common::Vec2f, 128> new_data;
            std::transform(lut.begin(), lut.end(), new_data.begin(), [](const auto& entry) {
                return Common::Vec2f{entry.ToFloat(), entry.DiffToFloat()};
            });

            if (new_data != lut_data || invalidate) {
                lut_data = new_data;
                std::memcpy(buffer + bytes_used, new_data.data(),
                            new_data.size() * sizeof(Common::Vec2f));
                lut_offset = static_cast<int>((offset + bytes_used) / sizeof(Common::Vec2f));
                uniform_block_data.dirty = true;
                bytes_used += new_data.size() * sizeof(Common::Vec2f);
            }
        };

    // Sync the proctex noise lut
    if (uniform_block_data.proctex_noise_lut_dirty || invalidate) {
        SyncProcTexValueLUT(Pica::g_state.proctex.noise_table, proctex_noise_lut_data,
                            uniform_block_data.data.proctex_noise_lut_offset);
        uniform_block_data.proctex_noise_lut_dirty = false;
    }

    // Sync the proctex color map
    if (uniform_block_data.proctex_color_map_dirty || invalidate) {
        SyncProcTexValueLUT(Pica::g_state.proctex.color_map_table, proctex_color_map_data,
                            uniform_block_data.data.proctex_color_map_offset);
        uniform_block_data.proctex_color_map_dirty = false;
    }

    // Sync the proctex alpha map
    if (uniform_block_data.proctex_alpha_map_dirty || invalidate) {
        SyncProcTexValueLUT(Pica::g_state.proctex.alpha_map_table, proctex_alpha_map_data,
                            uniform_block_data.data.proctex_alpha_map_offset);
        uniform_block_data.proctex_alpha_map_dirty = false;
    }

    // Sync the proctex lut
    if (uniform_block_data.proctex_lut_dirty || invalidate) {
        std::array<Common::Vec4f, 256> new_data;

        std::transform(Pica::g_state.proctex.color_table.begin(),
                       Pica::g_state.proctex.color_table.end(), new_data.begin(),
                       [](const auto& entry) {
                           auto rgba = entry.ToVector() / 255.0f;
                           return Common::Vec4f{rgba.r(), rgba.g(), rgba.b(), rgba.a()};
                       });

        if (new_data != proctex_lut_data || invalidate) {
            proctex_lut_data = new_data;
            std::memcpy(buffer + bytes_used, new_data.data(),
                        new_data.size() * sizeof(Common::Vec4f));
            uniform_block_data.data.proctex_lut_offset =
                static_cast<int>((offset + bytes_used) / sizeof(Common::Vec4f));
            uniform_block_data.dirty = true;
            bytes_used += new_data.size() * sizeof(Common::Vec4f);
        }
        uniform_block_data.proctex_lut_dirty = false;
    }

    // Sync the proctex difference lut
    if (uniform_block_data.proctex_diff_lut_dirty || invalidate) {
        std::array<Common::Vec4f, 256> new_data;

        std::transform(Pica::g_state.proctex.color_diff_table.begin(),
                       Pica::g_state.proctex.color_diff_table.end(), new_data.begin(),
                       [](const auto& entry) {
                           auto rgba = entry.ToVector() / 255.0f;
                           return Common::Vec4f{rgba.r(), rgba.g(), rgba.b(), rgba.a()};
                       });

        if (new_data != proctex_diff_lut_data || invalidate) {
            proctex_diff_lut_data = new_data;
            std::memcpy(buffer + bytes_used, new_data.data(),
                        new_data.size() * sizeof(Common::Vec4f));
            uniform_block_data.data.proctex_diff_lut_offset =
                static_cast<int>((offset + bytes_used) / sizeof(Common::Vec4f));
            uniform_block_data.dirty = true;
            bytes_used += new_data.size() * sizeof(Common::Vec4f);
        }
        uniform_block_data.proctex_diff_lut_dirty = false;
    }

    texture_buffer.Commit(static_cast<u32>(bytes_used));
}

void RasterizerVulkan::UploadUniforms(bool accelerate_draw) {
    const bool sync_vs = accelerate_draw;
    const bool sync_fs = uniform_block_data.dirty;

    if (!sync_vs && !sync_fs) {
        return;
    }

    u32 used_bytes = 0;
    const u32 uniform_size = static_cast<u32>(uniform_size_aligned_vs + uniform_size_aligned_fs);
    auto [uniforms, offset, invalidate] = uniform_buffer.Map(uniform_size);

    if (sync_vs) {
        Pica::Shader::VSUniformData vs_uniforms;
        vs_uniforms.uniforms.SetFromRegs(Pica::g_state.regs.vs, Pica::g_state.vs);
        std::memcpy(uniforms + used_bytes, &vs_uniforms, sizeof(vs_uniforms));

        pipeline_cache.BindBuffer(0, uniform_buffer.GetHandle(), offset + used_bytes,
                                  sizeof(vs_uniforms));
        used_bytes += static_cast<u32>(uniform_size_aligned_vs);
    }

    if (sync_fs || invalidate) {
        std::memcpy(uniforms + used_bytes, &uniform_block_data.data,
                    sizeof(Pica::Shader::UniformData));

        pipeline_cache.BindBuffer(1, uniform_buffer.GetHandle(), offset + used_bytes,
                                  sizeof(uniform_block_data.data));
        uniform_block_data.dirty = false;
        used_bytes += static_cast<u32>(uniform_size_aligned_fs);
    }

    uniform_buffer.Commit(used_bytes);
}

} // namespace Vulkan
