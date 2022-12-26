// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;
class RenderpassCache;

class Swapchain {
    static constexpr u32 PREFERRED_IMAGE_COUNT = 8;

public:
    Swapchain(const Instance& instance, Scheduler& scheduler, RenderpassCache& renderpass_cache);
    ~Swapchain();

    /// Creates (or recreates) the swapchain with a given size.
    void Create();

    /// Acquires the next image in the swapchain.
    void AcquireNextImage();

    /// Presents the current image and move to the next one
    void Present();

    /// Returns true when the swapchain should be recreated
    [[nodiscard]] bool NeedsRecreation() const {
        return is_suboptimal || is_outdated;
    }

    /// Returns current swapchain state
    [[nodiscard]] vk::Extent2D GetExtent() const {
        return extent;
    }

    /// Returns the swapchain surface
    [[nodiscard]] vk::SurfaceKHR GetSurface() const {
        return surface;
    }

    /// Returns the current framebuffe
    [[nodiscard]] vk::Framebuffer GetFramebuffer() const {
        return framebuffers[image_index];
    }

    /// Returns the swapchain format
    [[nodiscard]] vk::SurfaceFormatKHR GetSurfaceFormat() const {
        return surface_format;
    }

    /// Returns the Vulkan swapchain handle
    [[nodiscard]] vk::SwapchainKHR GetHandle() const {
        return swapchain;
    }

    [[nodiscard]] vk::Semaphore GetImageAcquiredSemaphore() const {
        return image_acquired[frame_index];
    }

    [[nodiscard]] vk::Semaphore GetPresentReadySemaphore() const {
        return present_ready[image_index];
    }

private:
    /// Selects the best available swapchain image format
    void FindPresentFormat();

    /// Sets the best available present mode
    void SetPresentMode();

    /// Sets the surface properties according to device capabilities
    void SetSurfaceProperties();

    /// Destroys current swapchain resources
    void Destroy();

    /// Performs creation of image views and framebuffers from the swapchain images
    void SetupImages();

private:
    const Instance& instance;
    Scheduler& scheduler;
    RenderpassCache& renderpass_cache;
    vk::SwapchainKHR swapchain{};
    vk::SurfaceKHR surface{};
    vk::SurfaceFormatKHR surface_format;
    vk::PresentModeKHR present_mode;
    vk::Extent2D extent;
    vk::SurfaceTransformFlagBitsKHR transform;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> image_views;
    std::vector<vk::Framebuffer> framebuffers;
    std::vector<u64> resource_ticks;
    std::vector<vk::Semaphore> image_acquired;
    std::vector<vk::Semaphore> present_ready;
    u32 image_count = 0;
    u32 image_index = 0;
    u32 frame_index = 0;
    bool is_outdated = true;
    bool is_suboptimal = true;
};

} // namespace Vulkan
