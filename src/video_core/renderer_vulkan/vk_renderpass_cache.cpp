// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_renderpass_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

VideoCore::PixelFormat ToFormatColor(u32 index) {
    switch (index) {
    case 0:
        return VideoCore::PixelFormat::RGBA8;
    case 1:
        return VideoCore::PixelFormat::RGB8;
    case 2:
        return VideoCore::PixelFormat::RGB5A1;
    case 3:
        return VideoCore::PixelFormat::RGB565;
    case 4:
        return VideoCore::PixelFormat::RGBA4;
    default:
        return VideoCore::PixelFormat::Invalid;
    }
}

VideoCore::PixelFormat ToFormatDepth(u32 index) {
    switch (index) {
    case 0:
        return VideoCore::PixelFormat::D16;
    case 2:
        return VideoCore::PixelFormat::D24;
    case 3:
        return VideoCore::PixelFormat::D24S8;
    default:
        return VideoCore::PixelFormat::Invalid;
    }
}

RenderpassCache::RenderpassCache(const Instance& instance, Scheduler& scheduler)
    : instance{instance}, scheduler{scheduler} {
    // Pre-create all needed renderpasses by the renderer
    for (u32 color = 0; color <= MAX_COLOR_FORMATS; color++) {
        for (u32 depth = 0; depth <= MAX_DEPTH_FORMATS; depth++) {
            const FormatTraits color_traits = instance.GetTraits(ToFormatColor(color));
            const FormatTraits depth_traits = instance.GetTraits(ToFormatDepth(depth));

            const vk::Format color_format = color_traits.transfer_support &&
                                                    color_traits.blit_support &&
                                                    color_traits.attachment_support
                                                ? color_traits.native
                                                : color_traits.fallback;
            const vk::Format depth_format =
                depth_traits.transfer_support && depth_traits.attachment_support
                    ? depth_traits.native
                    : depth_traits.fallback;

            if (color_format == vk::Format::eUndefined && depth_format == vk::Format::eUndefined) {
                continue;
            }

            cached_renderpasses[color][depth][0] =
                CreateRenderPass(color_format, depth_format, vk::AttachmentLoadOp::eLoad,
                                 vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral);
            cached_renderpasses[color][depth][1] =
                CreateRenderPass(color_format, depth_format, vk::AttachmentLoadOp::eClear,
                                 vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral);
        }
    }
}

RenderpassCache::~RenderpassCache() {
    vk::Device device = instance.GetDevice();
    for (u32 color = 0; color <= MAX_COLOR_FORMATS; color++) {
        for (u32 depth = 0; depth <= MAX_DEPTH_FORMATS; depth++) {
            if (vk::RenderPass load_pass = cached_renderpasses[color][depth][0]; load_pass) {
                device.destroyRenderPass(load_pass);
            }

            if (vk::RenderPass clear_pass = cached_renderpasses[color][depth][1]; clear_pass) {
                device.destroyRenderPass(clear_pass);
            }
        }
    }

    device.destroyRenderPass(present_renderpass);
}

void RenderpassCache::EnterRenderpass(const RenderpassState& state) {
    const bool is_dirty = scheduler.IsStateDirty(StateFlags::Renderpass);
    if (current_state == state && !is_dirty) {
        return;
    }

    scheduler.Record([should_end = bool(current_state.renderpass),
                      state](vk::CommandBuffer render_cmdbuf, vk::CommandBuffer) {
        if (should_end) {
            render_cmdbuf.endRenderPass();
        }

        const vk::RenderPassBeginInfo renderpass_begin_info = {
            .renderPass = state.renderpass,
            .framebuffer = state.framebuffer,
            .renderArea = state.render_area,
            .clearValueCount = 1,
            .pClearValues = &state.clear,
        };

        render_cmdbuf.beginRenderPass(renderpass_begin_info, vk::SubpassContents::eInline);
    });

    if (is_dirty) {
        scheduler.MarkStateNonDirty(StateFlags::Renderpass);
    }

    current_state = state;
}

void RenderpassCache::ExitRenderpass() {
    if (!current_state.renderpass) {
        return;
    }

    scheduler.Record(
        [](vk::CommandBuffer render_cmdbuf, vk::CommandBuffer) { render_cmdbuf.endRenderPass(); });

    current_state = {};
}

void RenderpassCache::CreatePresentRenderpass(vk::Format format) {
    if (!present_renderpass) {
        present_renderpass =
            CreateRenderPass(format, vk::Format::eUndefined, vk::AttachmentLoadOp::eClear,
                             vk::ImageLayout::eUndefined, vk::ImageLayout::ePresentSrcKHR);
    }
}

vk::RenderPass RenderpassCache::GetRenderpass(VideoCore::PixelFormat color,
                                              VideoCore::PixelFormat depth, bool is_clear) const {
    const u32 color_index =
        color == VideoCore::PixelFormat::Invalid ? MAX_COLOR_FORMATS : static_cast<u32>(color);
    const u32 depth_index = depth == VideoCore::PixelFormat::Invalid
                                ? MAX_DEPTH_FORMATS
                                : (static_cast<u32>(depth) - 14);

    ASSERT(color_index <= MAX_COLOR_FORMATS && depth_index <= MAX_DEPTH_FORMATS);
    return cached_renderpasses[color_index][depth_index][is_clear];
}

vk::RenderPass RenderpassCache::CreateRenderPass(vk::Format color, vk::Format depth,
                                                 vk::AttachmentLoadOp load_op,
                                                 vk::ImageLayout initial_layout,
                                                 vk::ImageLayout final_layout) const {
    u32 attachment_count = 0;
    std::array<vk::AttachmentDescription, 2> attachments;

    bool use_color = false;
    vk::AttachmentReference color_attachment_ref{};
    bool use_depth = false;
    vk::AttachmentReference depth_attachment_ref{};

    if (color != vk::Format::eUndefined) {
        attachments[attachment_count] = vk::AttachmentDescription{
            .format = color,
            .loadOp = load_op,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
            .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
            .initialLayout = initial_layout,
            .finalLayout = final_layout,
        };

        color_attachment_ref = vk::AttachmentReference{
            .attachment = attachment_count++,
            .layout = vk::ImageLayout::eGeneral,
        };

        use_color = true;
    }

    if (depth != vk::Format::eUndefined) {
        attachments[attachment_count] = vk::AttachmentDescription{
            .format = depth,
            .loadOp = load_op,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .stencilLoadOp = load_op,
            .stencilStoreOp = vk::AttachmentStoreOp::eStore,
            .initialLayout = vk::ImageLayout::eGeneral,
            .finalLayout = vk::ImageLayout::eGeneral,
        };

        depth_attachment_ref = vk::AttachmentReference{
            .attachment = attachment_count++,
            .layout = vk::ImageLayout::eGeneral,
        };

        use_depth = true;
    }

    // We also require only one subpass
    const vk::SubpassDescription subpass = {
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = use_color ? 1u : 0u,
        .pColorAttachments = &color_attachment_ref,
        .pResolveAttachments = 0,
        .pDepthStencilAttachment = use_depth ? &depth_attachment_ref : nullptr,
    };

    const vk::RenderPassCreateInfo renderpass_info = {
        .attachmentCount = attachment_count,
        .pAttachments = attachments.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = nullptr,
    };

    const vk::Device device = instance.GetDevice();
    return device.createRenderPass(renderpass_info);
}

} // namespace Vulkan
