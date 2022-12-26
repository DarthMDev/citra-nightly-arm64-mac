// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstring>
#include "video_core/rasterizer_cache/pixel_format.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

struct RenderpassState {
    vk::RenderPass renderpass;
    vk::Framebuffer framebuffer;
    vk::Rect2D render_area;
    vk::ClearValue clear;

    [[nodiscard]] bool operator==(const RenderpassState& other) const {
        return std::memcmp(this, &other, sizeof(RenderpassState)) == 0;
    }
};

class RenderpassCache {
    static constexpr u32 MAX_COLOR_FORMATS = 5;
    static constexpr u32 MAX_DEPTH_FORMATS = 4;

public:
    RenderpassCache(const Instance& instance, Scheduler& scheduler);
    ~RenderpassCache();

    /// Begins a new renderpass only when no other renderpass is currently active
    void EnterRenderpass(const RenderpassState& state);

    /// Exits from any currently active renderpass instance
    void ExitRenderpass();

    /// Creates the renderpass used when rendering to the swapchain
    void CreatePresentRenderpass(vk::Format format);

    /// Returns the renderpass associated with the color-depth format pair
    [[nodiscard]] vk::RenderPass GetRenderpass(VideoCore::PixelFormat color,
                                               VideoCore::PixelFormat depth, bool is_clear) const;

    /// Returns the swapchain clear renderpass
    [[nodiscard]] vk::RenderPass GetPresentRenderpass() const {
        return present_renderpass;
    }

private:
    /// Creates a renderpass configured appropriately and stores it in cached_renderpasses
    vk::RenderPass CreateRenderPass(vk::Format color, vk::Format depth,
                                    vk::AttachmentLoadOp load_op, vk::ImageLayout initial_layout,
                                    vk::ImageLayout final_layout) const;

private:
    const Instance& instance;
    Scheduler& scheduler;
    RenderpassState current_state{};
    vk::RenderPass present_renderpass{};
    vk::RenderPass cached_renderpasses[MAX_COLOR_FORMATS + 1][MAX_DEPTH_FORMATS + 1][2];
};

} // namespace Vulkan
