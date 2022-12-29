// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_vulkan/vk_descriptor_manager.h"
#include "video_core/renderer_vulkan/vk_format_reinterpreter.h"
#include "video_core/renderer_vulkan/vk_renderpass_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_texture_runtime.h"

namespace Vulkan {

D24S8toRGBA8::D24S8toRGBA8(const Instance& instance, Scheduler& scheduler,
                           DescriptorManager& desc_manager, TextureRuntime& runtime)
    : FormatReinterpreterBase{instance, scheduler, desc_manager, runtime},
      device{instance.GetDevice()} {
    constexpr std::string_view cs_source = R"(
#version 450 core
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform highp texture2D depth;
layout(set = 0, binding = 1) uniform lowp utexture2D stencil;
layout(set = 0, binding = 2, rgba8) uniform highp writeonly image2D color;
layout(push_constant, std140) uniform ComputeInfo {
mediump ivec2 src_offset;
};
void main() {
ivec2 tex_coord = src_offset + ivec2(gl_GlobalInvocationID.xy);
highp uint depth_val =
    uint(texelFetch(depth, tex_coord, 0).x * (exp2(32.0) - 1.0));
lowp uint stencil_val = texelFetch(stencil, tex_coord, 0).x;
highp uvec4 components =
    uvec4(stencil_val, (uvec3(depth_val) >> uvec3(24u, 16u, 8u)) & 0x000000FFu);
imageStore(color, tex_coord, vec4(components) / (exp2(8.0) - 1.0));
}
)";
    compute_shader =
        Compile(cs_source, vk::ShaderStageFlagBits::eCompute, device, ShaderOptimization::High);

    const std::array compute_layout_bindings = {
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    };

    const vk::DescriptorSetLayoutCreateInfo compute_layout_info = {
        .bindingCount = static_cast<u32>(compute_layout_bindings.size()),
        .pBindings = compute_layout_bindings.data(),
    };

    descriptor_layout = device.createDescriptorSetLayout(compute_layout_info);

    const std::array update_template_entries = {
        vk::DescriptorUpdateTemplateEntry{
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .offset = 0,
            .stride = sizeof(vk::DescriptorImageInfo),
        },
        vk::DescriptorUpdateTemplateEntry{
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .offset = sizeof(vk::DescriptorImageInfo),
        },
        vk::DescriptorUpdateTemplateEntry{
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .offset = 2 * sizeof(vk::DescriptorImageInfo),
        },
    };

    const vk::DescriptorUpdateTemplateCreateInfo template_info = {
        .descriptorUpdateEntryCount = static_cast<u32>(update_template_entries.size()),
        .pDescriptorUpdateEntries = update_template_entries.data(),
        .templateType = vk::DescriptorUpdateTemplateType::eDescriptorSet,
        .descriptorSetLayout = descriptor_layout,
    };

    update_template = device.createDescriptorUpdateTemplate(template_info);

    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Common::Vec2i),
    };

    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };

    compute_pipeline_layout = device.createPipelineLayout(layout_info);

    const vk::PipelineShaderStageCreateInfo compute_stage = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = compute_shader,
        .pName = "main",
    };

    const vk::ComputePipelineCreateInfo compute_info = {
        .stage = compute_stage,
        .layout = compute_pipeline_layout,
    };

    if (const auto result = device.createComputePipeline({}, compute_info);
        result.result == vk::Result::eSuccess) {
        compute_pipeline = result.value;
    } else {
        LOG_CRITICAL(Render_Vulkan, "D24S8 compute pipeline creation failed!");
        UNREACHABLE();
    }
}

D24S8toRGBA8::~D24S8toRGBA8() {
    device.destroyPipeline(compute_pipeline);
    device.destroyPipelineLayout(compute_pipeline_layout);
    device.destroyDescriptorUpdateTemplate(update_template);
    device.destroyDescriptorSetLayout(descriptor_layout);
    device.destroyShaderModule(compute_shader);
}

void D24S8toRGBA8::Reinterpret(Surface& source, VideoCore::Rect2D src_rect, Surface& dest,
                               VideoCore::Rect2D dst_rect) {
    const std::array textures = {
        vk::DescriptorImageInfo{
            .imageView = source.GetDepthView(),
            .imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .imageView = source.GetStencilView(),
            .imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .imageView = dest.GetImageView(),
            .imageLayout = vk::ImageLayout::eGeneral,
        },
    };

    vk::DescriptorSet set = desc_manager.AllocateSet(descriptor_layout);
    device.updateDescriptorSetWithTemplate(set, update_template, textures[0]);

    runtime.GetRenderpassCache().ExitRenderpass();
    scheduler.Record([this, set, src_rect, src_image = source.alloc.image,
                      dst_image = dest.alloc.image](vk::CommandBuffer render_cmdbuf,
                                                    vk::CommandBuffer) {
        const vk::ImageMemoryBarrier pre_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eShaderWrite |
                             vk::AccessFlagBits::eDepthStencilAttachmentWrite |
                             vk::AccessFlagBits::eDepthStencilAttachmentRead,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = src_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        const std::array post_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eShaderRead,
                .dstAccessMask = vk::AccessFlagBits::eShaderWrite |
                                 vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                .oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange{
                    .aspectMask =
                        vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            }};

        render_cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                      vk::PipelineStageFlagBits::eComputeShader,
                                      vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

        render_cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, compute_pipeline_layout,
                                         0, set, {});
        render_cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, compute_pipeline);

        const auto src_offset = Common::MakeVec(src_rect.left, src_rect.bottom);
        render_cmdbuf.pushConstants(compute_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                    sizeof(Common::Vec2i), src_offset.AsArray());

        render_cmdbuf.dispatch(src_rect.GetWidth() / 8, src_rect.GetHeight() / 8, 1);

        render_cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                      vk::PipelineStageFlagBits::eAllCommands,
                                      vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
    });
}

} // namespace Vulkan
