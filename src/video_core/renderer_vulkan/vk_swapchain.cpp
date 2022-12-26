// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_renderpass_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

namespace Vulkan {

Swapchain::Swapchain(const Instance& instance, Scheduler& scheduler,
                     RenderpassCache& renderpass_cache)
    : instance{instance}, scheduler{scheduler},
      renderpass_cache{renderpass_cache}, surface{instance.GetSurface()} {
    FindPresentFormat();
    SetPresentMode();
    renderpass_cache.CreatePresentRenderpass(surface_format.format);

    vk::Device device = instance.GetDevice();
    for (u32 i = 0; i < PREFERRED_IMAGE_COUNT; i++) {
        image_acquired.push_back(device.createSemaphore({}));
        present_ready.push_back(device.createSemaphore({}));
    }
}

Swapchain::~Swapchain() {
    Destroy();

    vk::Device device = instance.GetDevice();
    for (const vk::Semaphore semaphore : image_acquired) {
        device.destroySemaphore(semaphore);
    }
    for (const vk::Semaphore semaphore : present_ready) {
        device.destroySemaphore(semaphore);
    }
}

void Swapchain::Create() {
    is_outdated = false;
    is_suboptimal = false;
    SetSurfaceProperties();

    const std::array queue_family_indices = {
        instance.GetGraphicsQueueFamilyIndex(),
        instance.GetPresentQueueFamilyIndex(),
    };

    const bool exclusive = queue_family_indices[0] == queue_family_indices[1];
    const u32 queue_family_indices_count = exclusive ? 1u : 2u;
    const vk::SharingMode sharing_mode =
        exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent;
    const vk::SwapchainCreateInfoKHR swapchain_info = {
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
        .imageSharingMode = sharing_mode,
        .queueFamilyIndexCount = queue_family_indices_count,
        .pQueueFamilyIndices = queue_family_indices.data(),
        .preTransform = transform,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = swapchain};

    vk::Device device = instance.GetDevice();
    vk::SwapchainKHR new_swapchain = device.createSwapchainKHR(swapchain_info);
    device.waitIdle();
    Destroy();

    swapchain = new_swapchain;
    SetupImages();

    resource_ticks.clear();
    resource_ticks.resize(image_count);
}

MICROPROFILE_DEFINE(Vulkan_Acquire, "Vulkan", "Swapchain Acquire", MP_RGB(185, 66, 245));
void Swapchain::AcquireNextImage() {
    MICROPROFILE_SCOPE(Vulkan_Acquire);
    vk::Device device = instance.GetDevice();
    vk::Result result = device.acquireNextImageKHR(
        swapchain, UINT64_MAX, image_acquired[frame_index], VK_NULL_HANDLE, &image_index);

    switch (result) {
    case vk::Result::eSuccess:
        break;
    case vk::Result::eSuboptimalKHR:
        is_suboptimal = true;
        break;
    case vk::Result::eErrorOutOfDateKHR:
        is_outdated = true;
        break;
    default:
        LOG_ERROR(Render_Vulkan, "vkAcquireNextImageKHR returned unknown result {}", result);
        break;
    }

    scheduler.Wait(resource_ticks[image_index]);
    resource_ticks[image_index] = scheduler.CurrentTick();
}

MICROPROFILE_DEFINE(Vulkan_Present, "Vulkan", "Swapchain Present", MP_RGB(66, 185, 245));
void Swapchain::Present() {
    scheduler.Record([this, index = image_index](vk::CommandBuffer, vk::CommandBuffer) {
        const vk::PresentInfoKHR present_info = {.waitSemaphoreCount = 1,
                                                 .pWaitSemaphores = &present_ready[index],
                                                 .swapchainCount = 1,
                                                 .pSwapchains = &swapchain,
                                                 .pImageIndices = &index};
        MICROPROFILE_SCOPE(Vulkan_Present);
        vk::Queue present_queue = instance.GetPresentQueue();
        try {
            [[maybe_unused]] vk::Result result = present_queue.presentKHR(present_info);
        } catch (vk::OutOfDateKHRError&) {
            is_outdated = true;
        } catch (...) {
            LOG_CRITICAL(Render_Vulkan, "Swapchain presentation failed");
            UNREACHABLE();
        }
    });

    frame_index = (frame_index + 1) % image_count;
}

void Swapchain::FindPresentFormat() {
    const std::vector<vk::SurfaceFormatKHR> formats =
        instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface);

        // If there is a single undefined surface format, the device doesn't care, so we'll just use
        // RGBA
    if (formats[0].format == vk::Format::eUndefined) {
        surface_format.format = vk::Format::eR8G8B8A8Unorm;
        surface_format.colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
        return;
    }

    // Try to find a suitable format.
    for (const vk::SurfaceFormatKHR& sformat : formats) {
        vk::Format format = sformat.format;
        if (format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eB8G8R8A8Unorm) {
            continue;
        }

        surface_format.format = format;
        surface_format.colorSpace = sformat.colorSpace;
        return;
    }

    LOG_CRITICAL(Render_Vulkan, "Unable to find required swapchain format!");
    UNREACHABLE();
}

void Swapchain::SetPresentMode() {
    present_mode = vk::PresentModeKHR::eFifo;
    if (!Settings::values.use_vsync_new) {
        const std::vector<vk::PresentModeKHR> modes =
            instance.GetPhysicalDevice().getSurfacePresentModesKHR(surface);

        const auto FindMode = [&modes](vk::PresentModeKHR requested) {
            auto it =
                std::find_if(modes.begin(), modes.end(),
                             [&requested](vk::PresentModeKHR mode) { return mode == requested; });

            return it != modes.end();
        };

        // Prefer immediate when vsync is disabled for fastest acquire
        if (FindMode(vk::PresentModeKHR::eImmediate)) {
            present_mode = vk::PresentModeKHR::eImmediate;
        } else if (FindMode(vk::PresentModeKHR::eMailbox)) {
            present_mode = vk::PresentModeKHR::eMailbox;
        }
    }
}

void Swapchain::SetSurfaceProperties() {
    const vk::SurfaceCapabilitiesKHR capabilities =
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);

    extent = capabilities.currentExtent;
    if (capabilities.currentExtent.width == std::numeric_limits<u32>::max()) {
        LOG_CRITICAL(Render_Vulkan, "Device reported no surface extent");
        UNREACHABLE();
    }

    LOG_INFO(Render_Vulkan, "Creating {}x{} surface", extent.width, extent.height);

    // Select number of images in swap chain, we prefer one buffer in the background to work on
    image_count = PREFERRED_IMAGE_COUNT;
    if (capabilities.maxImageCount > 0) {
        image_count = std::clamp(PREFERRED_IMAGE_COUNT, capabilities.minImageCount + 1,
                                 capabilities.maxImageCount);
    }

    // Prefer identity transform if possible
    transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    if (!(capabilities.supportedTransforms & transform)) {
        transform = capabilities.currentTransform;
    }
}

void Swapchain::Destroy() {
    vk::Device device = instance.GetDevice();
    if (swapchain) {
        device.destroySwapchainKHR(swapchain);
    }
    for (const vk::ImageView view : image_views) {
        device.destroyImageView(view);
    }
    for (const vk::Framebuffer framebuffer : framebuffers) {
        device.destroyFramebuffer(framebuffer);
    }

    framebuffers.clear();
    image_views.clear();
}

void Swapchain::SetupImages() {
    vk::Device device = instance.GetDevice();
    images = device.getSwapchainImagesKHR(swapchain);

    for (const vk::Image image : images) {
        const vk::ImageViewCreateInfo view_info = {
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = surface_format.format,
            .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1}};

        image_views.push_back(device.createImageView(view_info));

        const vk::FramebufferCreateInfo framebuffer_info = {
            .renderPass = renderpass_cache.GetPresentRenderpass(),
            .attachmentCount = 1,
            .pAttachments = &image_views.back(),
            .width = extent.width,
            .height = extent.height,
            .layers = 1};

        framebuffers.push_back(device.createFramebuffer(framebuffer_info));
    }
}

} // namespace Vulkan
