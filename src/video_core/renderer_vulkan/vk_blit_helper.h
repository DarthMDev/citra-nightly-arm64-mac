// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {
struct TextureBlit;
}

namespace Vulkan {

class Instance;
class DescriptorManager;
class Scheduler;
class Surface;

class BlitHelper {
public:
    BlitHelper(const Instance& instance, Scheduler& scheduler, DescriptorManager& desc_manager);
    ~BlitHelper();

    /// Blits D24S8 pixel data to the provided buffer
    void BlitD24S8ToR32(Surface& depth_surface, Surface& r32_surface,
                        const VideoCore::TextureBlit& blit);

private:
    Scheduler& scheduler;
    DescriptorManager& desc_manager;
    vk::Device device;
    vk::Pipeline compute_pipeline;
    vk::PipelineLayout compute_pipeline_layout;
    vk::DescriptorSetLayout descriptor_layout;
    vk::DescriptorUpdateTemplate update_template;
    vk::ShaderModule compute_shader;
};

} // namespace Vulkan
