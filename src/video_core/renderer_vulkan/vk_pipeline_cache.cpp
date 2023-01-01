// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <filesystem>
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/pica_to_vk.h"
#include "video_core/renderer_vulkan/vk_descriptor_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_renderpass_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

u32 AttribBytes(Pica::PipelineRegs::VertexAttributeFormat format, u32 size) {
    switch (format) {
    case Pica::PipelineRegs::VertexAttributeFormat::FLOAT:
        return sizeof(float) * size;
    case Pica::PipelineRegs::VertexAttributeFormat::SHORT:
        return sizeof(u16) * size;
    case Pica::PipelineRegs::VertexAttributeFormat::BYTE:
    case Pica::PipelineRegs::VertexAttributeFormat::UBYTE:
        return sizeof(u8) * size;
    }

    return 0;
}

vk::Format ToVkAttributeFormat(Pica::PipelineRegs::VertexAttributeFormat format, u32 size) {
    static constexpr std::array attribute_formats = {
        std::array{
            vk::Format::eR8Sint,
            vk::Format::eR8G8Sint,
            vk::Format::eR8G8B8Sint,
            vk::Format::eR8G8B8A8Sint,
        },
        std::array{
            vk::Format::eR8Uint,
            vk::Format::eR8G8Uint,
            vk::Format::eR8G8B8Uint,
            vk::Format::eR8G8B8A8Uint,
        },
        std::array{
            vk::Format::eR16Sint,
            vk::Format::eR16G16Sint,
            vk::Format::eR16G16B16Sint,
            vk::Format::eR16G16B16A16Sint,
        },
        std::array{
            vk::Format::eR32Sfloat,
            vk::Format::eR32G32Sfloat,
            vk::Format::eR32G32B32Sfloat,
            vk::Format::eR32G32B32A32Sfloat,
        },
    };

    ASSERT(size <= 4);
    return attribute_formats[static_cast<u32>(format)][size - 1];
}

vk::ShaderStageFlagBits ToVkShaderStage(std::size_t index) {
    switch (index) {
    case 0:
        return vk::ShaderStageFlagBits::eVertex;
    case 1:
        return vk::ShaderStageFlagBits::eFragment;
    case 2:
        return vk::ShaderStageFlagBits::eGeometry;
    default:
        LOG_CRITICAL(Render_Vulkan, "Invalid shader stage index!");
        UNREACHABLE();
    }

    return vk::ShaderStageFlagBits::eVertex;
}

[[nodiscard]] bool IsAttribFormatSupported(const VertexAttribute& attrib,
                                           const Instance& instance) {
    static std::unordered_map<vk::Format, bool> format_support_cache;

    vk::PhysicalDevice physical_device = instance.GetPhysicalDevice();
    const vk::Format format = ToVkAttributeFormat(attrib.type, attrib.size);
    auto [it, new_format] = format_support_cache.try_emplace(format, false);
    if (new_format) {
        LOG_INFO(Render_Vulkan, "Quering support for format {}", vk::to_string(format));
        const vk::FormatFeatureFlags features =
            physical_device.getFormatProperties(format).bufferFeatures;
        it->second = (features & vk::FormatFeatureFlagBits::eVertexBuffer) ==
                     vk::FormatFeatureFlagBits::eVertexBuffer;
    }

    return it->second;
};

PipelineCache::PipelineCache(const Instance& instance, Scheduler& scheduler,
                             RenderpassCache& renderpass_cache, DescriptorManager& desc_manager)
    : instance{instance}, scheduler{scheduler}, renderpass_cache{renderpass_cache},
      desc_manager{desc_manager} {
    trivial_vertex_shader = Compile(GenerateTrivialVertexShader(), vk::ShaderStageFlagBits::eVertex,
                                    instance.GetDevice(), ShaderOptimization::Debug);
}

PipelineCache::~PipelineCache() {
    vk::Device device = instance.GetDevice();

    SaveDiskCache();

    device.destroyPipelineCache(pipeline_cache);
    device.destroyShaderModule(trivial_vertex_shader);

    for (auto& [key, module] : programmable_vertex_shaders.shader_cache) {
        device.destroyShaderModule(module);
    }

    for (auto& [key, module] : fixed_geometry_shaders.shaders) {
        device.destroyShaderModule(module);
    }

    for (auto& [key, module] : fragment_shaders_glsl.shaders) {
        device.destroyShaderModule(module);
    }

    for (auto& [key, module] : fragment_shaders_spv.shaders) {
        device.destroyShaderModule(module);
    }

    for (const auto& [hash, pipeline] : graphics_pipelines) {
        device.destroyPipeline(pipeline);
    }

    graphics_pipelines.clear();
}

void PipelineCache::LoadDiskCache() {
    if (!Settings::values.use_disk_shader_cache || !EnsureDirectories()) {
        return;
    }

    const std::string cache_file_path = fmt::format("{}{:x}{:x}.bin", GetPipelineCacheDir(),
                                                    instance.GetVendorID(), instance.GetDeviceID());
    vk::PipelineCacheCreateInfo cache_info = {
        .initialDataSize = 0,
        .pInitialData = nullptr,
    };

    std::vector<u8> cache_data;
    FileUtil::IOFile cache_file{cache_file_path, "r"};
    if (cache_file.IsOpen()) {
        LOG_INFO(Render_Vulkan, "Loading pipeline cache");

        const u64 cache_file_size = cache_file.GetSize();
        cache_data.resize(cache_file_size);
        if (cache_file.ReadBytes(cache_data.data(), cache_file_size)) {
            if (!IsCacheValid(cache_data.data(), cache_file_size)) {
                LOG_WARNING(Render_Vulkan, "Pipeline cache provided invalid, ignoring");
            } else {
                cache_info.initialDataSize = cache_file_size;
                cache_info.pInitialData = cache_data.data();
            }
        }

        cache_file.Close();
    }

    vk::Device device = instance.GetDevice();
    pipeline_cache = device.createPipelineCache(cache_info);
}

void PipelineCache::SaveDiskCache() {
    if (!Settings::values.use_disk_shader_cache || !EnsureDirectories()) {
        return;
    }

    const std::string cache_file_path = fmt::format("{}{:x}{:x}.bin", GetPipelineCacheDir(),
                                                    instance.GetVendorID(), instance.GetDeviceID());
    FileUtil::IOFile cache_file{cache_file_path, "wb"};
    if (!cache_file.IsOpen()) {
        LOG_INFO(Render_Vulkan, "Unable to open pipeline cache for writing");
        return;
    }

    vk::Device device = instance.GetDevice();
    auto cache_data = device.getPipelineCacheData(pipeline_cache);
    if (!cache_file.WriteBytes(cache_data.data(), cache_data.size())) {
        LOG_WARNING(Render_Vulkan, "Error during pipeline cache write");
        return;
    }

    cache_file.Close();
}

void PipelineCache::BindPipeline(const PipelineInfo& info) {
    ApplyDynamic(info);

    scheduler.Record([this, info](vk::CommandBuffer cmdbuf) {
        std::size_t shader_hash = 0;
        for (u32 i = 0; i < MAX_SHADER_STAGES; i++) {
            shader_hash = Common::HashCombine(shader_hash, shader_hashes[i]);
        }

        const u64 info_hash_size = instance.IsExtendedDynamicStateSupported()
                                       ? offsetof(PipelineInfo, rasterization)
                                       : offsetof(PipelineInfo, dynamic);

        u64 info_hash = Common::ComputeHash64(&info, info_hash_size);
        u64 pipeline_hash = Common::HashCombine(shader_hash, info_hash);

        auto [it, new_pipeline] = graphics_pipelines.try_emplace(pipeline_hash, vk::Pipeline{});
        if (new_pipeline) {
            it->second = BuildPipeline(info);
        }

        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, it->second);
        current_pipeline = it->second;
    });

    desc_manager.BindDescriptorSets();
}

bool PipelineCache::UseProgrammableVertexShader(const Pica::Regs& regs,
                                                Pica::Shader::ShaderSetup& setup,
                                                const VertexLayout& layout) {
    PicaVSConfig config{regs.rasterizer, regs.vs, setup};
    config.state.use_geometry_shader = instance.UseGeometryShaders();

    u32 emulated_attrib_loc = MAX_VERTEX_ATTRIBUTES;
    for (u32 i = 0; i < layout.attribute_count; i++) {
        const auto& attrib = layout.attributes[i];
        const u32 location = attrib.location.Value();
        const bool is_supported = IsAttribFormatSupported(attrib, instance);
        ASSERT(is_supported || attrib.size == 3);

        config.state.attrib_types[location] = attrib.type.Value();
        config.state.emulated_attrib_locations[location] = is_supported ? 0 : emulated_attrib_loc++;
    }

    auto [handle, result] =
        programmable_vertex_shaders.Get(config, setup, vk::ShaderStageFlagBits::eVertex,
                                        instance.GetDevice(), ShaderOptimization::High);
    if (!handle) {
        LOG_ERROR(Render_Vulkan, "Failed to retrieve programmable vertex shader");
        return false;
    }

    scheduler.Record(
        [this, handle = handle, hash = config.Hash()](vk::CommandBuffer) {
            current_shaders[ProgramType::VS] = handle;
            shader_hashes[ProgramType::VS] = hash;
        });

    return true;
}

void PipelineCache::UseTrivialVertexShader() {
    scheduler.Record([this](vk::CommandBuffer) {
        current_shaders[ProgramType::VS] = trivial_vertex_shader;
        shader_hashes[ProgramType::VS] = 0;
    });
}

void PipelineCache::UseFixedGeometryShader(const Pica::Regs& regs) {
    if (!instance.UseGeometryShaders()) {
        return UseTrivialGeometryShader();
    }

    const PicaFixedGSConfig gs_config{regs};
    const vk::ShaderModule handle =
        fixed_geometry_shaders.Get(gs_config, vk::ShaderStageFlagBits::eGeometry,
                                   instance.GetDevice(), ShaderOptimization::Debug);

    scheduler.Record([this, handle, hash = gs_config.Hash()](vk::CommandBuffer) {
        current_shaders[ProgramType::GS] = handle;
        shader_hashes[ProgramType::GS] = hash;
    });
}

void PipelineCache::UseTrivialGeometryShader() {
    scheduler.Record([this](vk::CommandBuffer) {
        current_shaders[ProgramType::GS] = VK_NULL_HANDLE;
        shader_hashes[ProgramType::GS] = 0;
    });
}

MICROPROFILE_DEFINE(Vulkan_FragmentGeneration, "Vulkan", "Fragment Shader Compilation",
                    MP_RGB(255, 100, 100));
void PipelineCache::UseFragmentShader(const Pica::Regs& regs) {
    const PicaFSConfig config{regs, instance};

    scheduler.Record([this, config](vk::CommandBuffer) {
        MICROPROFILE_SCOPE(Vulkan_FragmentGeneration);

        vk::ShaderModule handle{};
        if (Settings::values.spirv_shader_gen) {
            handle = fragment_shaders_spv.Get(config, instance.GetDevice());
        } else {
            handle = fragment_shaders_glsl.Get(config, vk::ShaderStageFlagBits::eFragment,
                                               instance.GetDevice(), ShaderOptimization::High);
        }

        current_shaders[ProgramType::FS] = handle;
        shader_hashes[ProgramType::FS] = config.Hash();
    });
}

void PipelineCache::BindTexture(u32 binding, vk::ImageView image_view) {
    const vk::DescriptorImageInfo image_info = {
        .imageView = image_view,
        .imageLayout = vk::ImageLayout::eGeneral,
    };
    desc_manager.SetBinding(1, binding, DescriptorData{image_info});
}

void PipelineCache::BindStorageImage(u32 binding, vk::ImageView image_view) {
    const vk::DescriptorImageInfo image_info = {
        .imageView = image_view,
        .imageLayout = vk::ImageLayout::eGeneral,
    };
    desc_manager.SetBinding(3, binding, DescriptorData{image_info});
}

void PipelineCache::BindBuffer(u32 binding, vk::Buffer buffer, u32 offset, u32 size) {
    const DescriptorData data = {
        .buffer_info =
            vk::DescriptorBufferInfo{
                .buffer = buffer,
                .offset = offset,
                .range = size,
            },
    };
    desc_manager.SetBinding(0, binding, data);
}

void PipelineCache::BindTexelBuffer(u32 binding, vk::BufferView buffer_view) {
    const DescriptorData data = {
        .buffer_view = buffer_view,
    };
    desc_manager.SetBinding(0, binding, data);
}

void PipelineCache::BindSampler(u32 binding, vk::Sampler sampler) {
    const DescriptorData data = {
        .image_info =
            vk::DescriptorImageInfo{
                .sampler = sampler,
            },
    };
    desc_manager.SetBinding(2, binding, data);
}

void PipelineCache::SetViewport(float x, float y, float width, float height) {
    const vk::Viewport viewport{x, y, width, height, 0.f, 1.f};
    scheduler.Record([viewport](vk::CommandBuffer cmdbuf) {
        cmdbuf.setViewport(0, viewport);
    });
}

void PipelineCache::SetScissor(s32 x, s32 y, u32 width, u32 height) {
    const vk::Rect2D scissor{{x, y}, {width, height}};
    scheduler.Record([scissor](vk::CommandBuffer cmdbuf) {
        cmdbuf.setScissor(0, scissor);
    });
}

void PipelineCache::ApplyDynamic(const PipelineInfo& info) {
    const bool is_dirty = scheduler.IsStateDirty(StateFlags::Pipeline);

    PipelineInfo current = current_info;
    scheduler.Record([this, info, is_dirty, current](vk::CommandBuffer cmdbuf) {
        if (info.dynamic.stencil_compare_mask != current.dynamic.stencil_compare_mask || is_dirty) {
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                                info.dynamic.stencil_compare_mask);
        }

        if (info.dynamic.stencil_write_mask != current.dynamic.stencil_write_mask || is_dirty) {
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                              info.dynamic.stencil_write_mask);
        }

        if (info.dynamic.stencil_reference != current.dynamic.stencil_reference || is_dirty) {
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                              info.dynamic.stencil_reference);
        }

        if (info.dynamic.blend_color != current.dynamic.blend_color || is_dirty) {
            const Common::Vec4f color = PicaToVK::ColorRGBA8(info.dynamic.blend_color);
            cmdbuf.setBlendConstants(color.AsArray());
        }

        if (instance.IsExtendedDynamicStateSupported()) {
            if (info.rasterization.cull_mode != current.rasterization.cull_mode || is_dirty) {
                cmdbuf.setCullModeEXT(PicaToVK::CullMode(info.rasterization.cull_mode));
                cmdbuf.setFrontFaceEXT(PicaToVK::FrontFace(info.rasterization.cull_mode));
            }

            if (info.depth_stencil.depth_compare_op != current.depth_stencil.depth_compare_op ||
                is_dirty) {
                cmdbuf.setDepthCompareOpEXT(
                    PicaToVK::CompareFunc(info.depth_stencil.depth_compare_op));
            }

            if (info.depth_stencil.depth_test_enable != current.depth_stencil.depth_test_enable ||
                is_dirty) {
                cmdbuf.setDepthTestEnableEXT(info.depth_stencil.depth_test_enable);
            }

            if (info.depth_stencil.depth_write_enable != current.depth_stencil.depth_write_enable ||
                is_dirty) {
                cmdbuf.setDepthWriteEnableEXT(info.depth_stencil.depth_write_enable);
            }

            if (info.rasterization.topology != current.rasterization.topology || is_dirty) {
                cmdbuf.setPrimitiveTopologyEXT(
                    PicaToVK::PrimitiveTopology(info.rasterization.topology));
            }

            if (info.depth_stencil.stencil_test_enable !=
                    current.depth_stencil.stencil_test_enable ||
                is_dirty) {
                cmdbuf.setStencilTestEnableEXT(info.depth_stencil.stencil_test_enable);
            }

            if (info.depth_stencil.stencil_fail_op != current.depth_stencil.stencil_fail_op ||
                info.depth_stencil.stencil_pass_op != current.depth_stencil.stencil_pass_op ||
                info.depth_stencil.stencil_depth_fail_op !=
                    current.depth_stencil.stencil_depth_fail_op ||
                info.depth_stencil.stencil_compare_op != current.depth_stencil.stencil_compare_op ||
                is_dirty) {
                cmdbuf.setStencilOpEXT(
                    vk::StencilFaceFlagBits::eFrontAndBack,
                    PicaToVK::StencilOp(info.depth_stencil.stencil_fail_op),
                    PicaToVK::StencilOp(info.depth_stencil.stencil_pass_op),
                    PicaToVK::StencilOp(info.depth_stencil.stencil_depth_fail_op),
                    PicaToVK::CompareFunc(info.depth_stencil.stencil_compare_op));
            }
        }
    });

    current_info = info;
    if (is_dirty) {
        scheduler.MarkStateNonDirty(StateFlags::Pipeline);
    }
}

vk::Pipeline PipelineCache::BuildPipeline(const PipelineInfo& info) {
    const vk::Device device = instance.GetDevice();

    u32 shader_count = 0;
    std::array<vk::PipelineShaderStageCreateInfo, MAX_SHADER_STAGES> shader_stages;
    for (std::size_t i = 0; i < current_shaders.size(); i++) {
        const vk::ShaderModule shader = current_shaders[i];
        if (!shader) {
            continue;
        }

        shader_stages[shader_count++] = vk::PipelineShaderStageCreateInfo{
            .stage = ToVkShaderStage(i),
            .module = shader,
            .pName = "main",
        };
    }

    /**
     * Vulkan doesn't intuitively support fixed attributes. To avoid duplicating the data and
     * increasing data upload, when the fixed flag is true, we specify VK_VERTEX_INPUT_RATE_INSTANCE
     * as the input rate. Since one instance is all we render, the shader will always read the
     * single attribute.
     **/
    std::array<vk::VertexInputBindingDescription, MAX_VERTEX_BINDINGS> bindings;
    for (u32 i = 0; i < info.vertex_layout.binding_count; i++) {
        const auto& binding = info.vertex_layout.bindings[i];
        bindings[i] = vk::VertexInputBindingDescription{
            .binding = binding.binding,
            .stride = binding.stride,
            .inputRate = binding.fixed.Value() ? vk::VertexInputRate::eInstance
                                               : vk::VertexInputRate::eVertex,
        };
    }

    u32 emulated_attrib_count = 0;
    std::array<vk::VertexInputAttributeDescription, MAX_VERTEX_ATTRIBUTES * 2> attributes;
    for (u32 i = 0; i < info.vertex_layout.attribute_count; i++) {
        const VertexAttribute& attrib = info.vertex_layout.attributes[i];
        const vk::Format format = ToVkAttributeFormat(attrib.type, attrib.size);
        const bool is_supported = IsAttribFormatSupported(attrib, instance);
        ASSERT_MSG(is_supported || attrib.size == 3);

        attributes[i] = vk::VertexInputAttributeDescription{
            .location = attrib.location,
            .binding = attrib.binding,
            .format = is_supported ? format : ToVkAttributeFormat(attrib.type, 2),
            .offset = attrib.offset,
        };

        // When the requested 3-component vertex format is unsupported by the hardware
        // is it emulated by breaking it into a vec2 + vec1. These are combined to a vec3
        // by the vertex shader.
        if (!is_supported) {
            const u32 location = MAX_VERTEX_ATTRIBUTES + emulated_attrib_count++;
            LOG_WARNING(Render_Vulkan, "\nEmulating attrib {} at location {}\n", attrib.location,
                        location);
            attributes[location] = vk::VertexInputAttributeDescription{
                .location = location,
                .binding = attrib.binding,
                .format = ToVkAttributeFormat(attrib.type, 1),
                .offset = attrib.offset + AttribBytes(attrib.type, 2),
            };
        }
    }

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .vertexBindingDescriptionCount = info.vertex_layout.binding_count,
        .pVertexBindingDescriptions = bindings.data(),
        .vertexAttributeDescriptionCount =
            info.vertex_layout.attribute_count + emulated_attrib_count,
        .pVertexAttributeDescriptions = attributes.data(),
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = PicaToVK::PrimitiveTopology(info.rasterization.topology),
        .primitiveRestartEnable = false,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state = {
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .cullMode = PicaToVK::CullMode(info.rasterization.cull_mode),
        .frontFace = PicaToVK::FrontFace(info.rasterization.cull_mode),
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = false,
    };

    const vk::PipelineColorBlendAttachmentState colorblend_attachment = {
        .blendEnable = info.blending.blend_enable.Value(),
        .srcColorBlendFactor = PicaToVK::BlendFunc(info.blending.src_color_blend_factor),
        .dstColorBlendFactor = PicaToVK::BlendFunc(info.blending.dst_color_blend_factor),
        .colorBlendOp = PicaToVK::BlendEquation(info.blending.color_blend_eq),
        .srcAlphaBlendFactor = PicaToVK::BlendFunc(info.blending.src_alpha_blend_factor),
        .dstAlphaBlendFactor = PicaToVK::BlendFunc(info.blending.dst_alpha_blend_factor),
        .alphaBlendOp = PicaToVK::BlendEquation(info.blending.alpha_blend_eq),
        .colorWriteMask = static_cast<vk::ColorComponentFlags>(info.blending.color_write_mask),
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = !info.blending.blend_enable.Value() && !instance.NeedsLogicOpEmulation(),
        .logicOp = PicaToVK::LogicOp(info.blending.logic_op.Value()),
        .attachmentCount = 1,
        .pAttachments = &colorblend_attachment,
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    const vk::Viewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = 1.0f,
        .height = 1.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {1, 1},
    };

    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    const bool extended_dynamic_states = instance.IsExtendedDynamicStateSupported();
    const std::array dynamic_states = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
        vk::DynamicState::eStencilCompareMask,
        vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilReference,
        vk::DynamicState::eBlendConstants,
        // VK_EXT_extended_dynamic_state
        vk::DynamicState::eCullModeEXT,
        vk::DynamicState::eDepthCompareOpEXT,
        vk::DynamicState::eDepthTestEnableEXT,
        vk::DynamicState::eDepthWriteEnableEXT,
        vk::DynamicState::eFrontFaceEXT,
        vk::DynamicState::ePrimitiveTopologyEXT,
        vk::DynamicState::eStencilOpEXT,
        vk::DynamicState::eStencilTestEnableEXT,
    };

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = extended_dynamic_states ? static_cast<u32>(dynamic_states.size()) : 6u,
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::StencilOpState stencil_op_state = {
        .failOp = PicaToVK::StencilOp(info.depth_stencil.stencil_fail_op),
        .passOp = PicaToVK::StencilOp(info.depth_stencil.stencil_pass_op),
        .depthFailOp = PicaToVK::StencilOp(info.depth_stencil.stencil_depth_fail_op),
        .compareOp = PicaToVK::CompareFunc(info.depth_stencil.stencil_compare_op),
    };

    const vk::PipelineDepthStencilStateCreateInfo depth_info = {
        .depthTestEnable = static_cast<u32>(info.depth_stencil.depth_test_enable.Value()),
        .depthWriteEnable = static_cast<u32>(info.depth_stencil.depth_write_enable.Value()),
        .depthCompareOp = PicaToVK::CompareFunc(info.depth_stencil.depth_compare_op),
        .depthBoundsTestEnable = false,
        .stencilTestEnable = static_cast<u32>(info.depth_stencil.stencil_test_enable.Value()),
        .front = stencil_op_state,
        .back = stencil_op_state,
    };

    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .stageCount = shader_count,
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_info,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = desc_manager.GetPipelineLayout(),
        .renderPass =
            renderpass_cache.GetRenderpass(info.color_attachment, info.depth_attachment, false),
    };

    if (const auto result = device.createGraphicsPipeline(pipeline_cache, pipeline_info);
        result.result == vk::Result::eSuccess) {
        return result.value;
    } else {
        LOG_CRITICAL(Render_Vulkan, "Graphics pipeline creation failed!");
        UNREACHABLE();
    }

    return VK_NULL_HANDLE;
}

bool PipelineCache::IsCacheValid(const u8* data, u64 size) const {
    if (size < sizeof(vk::PipelineCacheHeaderVersionOne)) {
        LOG_ERROR(Render_Vulkan, "Pipeline cache failed validation: Invalid header");
        return false;
    }

    vk::PipelineCacheHeaderVersionOne header;
    std::memcpy(&header, data, sizeof(header));
    if (header.headerSize < sizeof(header)) {
        LOG_ERROR(Render_Vulkan, "Pipeline cache failed validation: Invalid header length");
        return false;
    }

    if (header.headerVersion != vk::PipelineCacheHeaderVersion::eOne) {
        LOG_ERROR(Render_Vulkan, "Pipeline cache failed validation: Invalid header version");
        return false;
    }

    if (u32 vendor_id = instance.GetVendorID(); header.vendorID != vendor_id) {
        LOG_ERROR(
            Render_Vulkan,
            "Pipeline cache failed validation: Incorrect vendor ID (file: {:#X}, device: {:#X})",
            header.vendorID, vendor_id);
        return false;
    }

    if (u32 device_id = instance.GetDeviceID(); header.deviceID != device_id) {
        LOG_ERROR(
            Render_Vulkan,
            "Pipeline cache failed validation: Incorrect device ID (file: {:#X}, device: {:#X})",
            header.deviceID, device_id);
        return false;
    }

    if (header.pipelineCacheUUID != instance.GetPipelineCacheUUID()) {
        LOG_ERROR(Render_Vulkan, "Pipeline cache failed validation: Incorrect UUID");
        return false;
    }

    return true;
}

bool PipelineCache::EnsureDirectories() const {
    const auto CreateDir = [](const std::string& dir) {
        if (!FileUtil::CreateDir(dir)) {
            LOG_ERROR(Render_Vulkan, "Failed to create directory={}", dir);
            return false;
        }

        return true;
    };

    return CreateDir(FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir)) &&
           CreateDir(GetPipelineCacheDir());
}

std::string PipelineCache::GetPipelineCacheDir() const {
    return FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) + "vulkan" + DIR_SEP;
}

} // namespace Vulkan
