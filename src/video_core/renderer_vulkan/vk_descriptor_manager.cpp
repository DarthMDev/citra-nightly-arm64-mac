// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_vulkan/vk_descriptor_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "vulkan/vulkan.hpp"

namespace Vulkan {

struct Bindings {
    std::array<vk::DescriptorType, MAX_DESCRIPTORS> bindings;
    u32 binding_count;
};

constexpr static std::array RASTERIZER_SETS = {
    Bindings{// Utility set
             .bindings = {vk::DescriptorType::eUniformBuffer, vk::DescriptorType::eUniformBuffer,
                          vk::DescriptorType::eUniformTexelBuffer,
                          vk::DescriptorType::eUniformTexelBuffer,
                          vk::DescriptorType::eUniformTexelBuffer},
             .binding_count = 5},
    Bindings{// Texture set
             .bindings = {vk::DescriptorType::eSampledImage, vk::DescriptorType::eSampledImage,
                          vk::DescriptorType::eSampledImage, vk::DescriptorType::eSampledImage},
             .binding_count = 4},
    Bindings{// Sampler set
             .bindings = {vk::DescriptorType::eSampler, vk::DescriptorType::eSampler,
                          vk::DescriptorType::eSampler, vk::DescriptorType::eSampler},
             .binding_count = 4},
    Bindings{// Shadow set
             .bindings = {vk::DescriptorType::eStorageImage, vk::DescriptorType::eStorageImage,
                          vk::DescriptorType::eStorageImage, vk::DescriptorType::eStorageImage,
                          vk::DescriptorType::eStorageImage, vk::DescriptorType::eStorageImage,
                          vk::DescriptorType::eStorageImage},
             .binding_count = 7}};

constexpr vk::ShaderStageFlags ToVkStageFlags(vk::DescriptorType type) {
    vk::ShaderStageFlags flags;
    switch (type) {
    case vk::DescriptorType::eSampler:
    case vk::DescriptorType::eSampledImage:
    case vk::DescriptorType::eUniformTexelBuffer:
    case vk::DescriptorType::eStorageImage:
        flags = vk::ShaderStageFlagBits::eFragment;
        break;
    case vk::DescriptorType::eUniformBuffer:
    case vk::DescriptorType::eUniformBufferDynamic:
        flags = vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex |
                vk::ShaderStageFlagBits::eGeometry | vk::ShaderStageFlagBits::eCompute;
        break;
    default:
        LOG_ERROR(Render_Vulkan, "Unknown descriptor type!");
    }

    return flags;
}

DescriptorManager::DescriptorManager(const Instance& instance, Scheduler& scheduler)
    : instance{instance}, scheduler{scheduler}, pool_provider{instance,
                                                              scheduler.GetMasterSemaphore()} {
    BuildLayouts();
    descriptor_set_dirty.fill(true);
    current_pool = pool_provider.Commit();
}

DescriptorManager::~DescriptorManager() {
    vk::Device device = instance.GetDevice();
    device.destroyPipelineLayout(pipeline_layout);

    for (u32 i = 0; i < MAX_DESCRIPTOR_SETS; i++) {
        device.destroyDescriptorSetLayout(descriptor_set_layouts[i]);
        device.destroyDescriptorUpdateTemplate(update_templates[i]);
    }
}

void DescriptorManager::SetBinding(u32 set, u32 binding, DescriptorData data) {
    DescriptorData& current = update_data[set][binding];
    if (current != data) {
        current = data;
        descriptor_set_dirty[set] = true;
    }
}

void DescriptorManager::BindDescriptorSets() {
    const bool is_dirty = scheduler.IsStateDirty(StateFlags::DescriptorSets);
    if (is_dirty) {
        descriptor_set_dirty.fill(true);
    }

    vk::Device device = instance.GetDevice();
    std::array<vk::DescriptorSet, MAX_DESCRIPTOR_SETS> bound_sets;
    for (u32 i = 0; i < MAX_DESCRIPTOR_SETS; i++) {
        if (descriptor_set_dirty[i]) {
            vk::DescriptorSet set = AllocateSet(descriptor_set_layouts[i]);
            device.updateDescriptorSetWithTemplate(set, update_templates[i], update_data[i][0]);
            descriptor_sets[i] = set;
        }

        bound_sets[i] = descriptor_sets[i];
    }

    scheduler.Record([this, bound_sets](vk::CommandBuffer render_cmdbuf, vk::CommandBuffer) {
        render_cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0,
                                         bound_sets, {});
    });

    descriptor_set_dirty.fill(false);
    if (is_dirty) {
        scheduler.MarkStateNonDirty(StateFlags::DescriptorSets);
    }
}

void DescriptorManager::BuildLayouts() {
    std::array<vk::DescriptorSetLayoutBinding, MAX_DESCRIPTORS> set_bindings;
    std::array<vk::DescriptorUpdateTemplateEntry, MAX_DESCRIPTORS> update_entries;

    vk::Device device = instance.GetDevice();
    for (u32 i = 0; i < MAX_DESCRIPTOR_SETS; i++) {
        const auto& set = RASTERIZER_SETS[i];
        for (u32 j = 0; j < set.binding_count; j++) {
            vk::DescriptorType type = set.bindings[j];
            set_bindings[j] = vk::DescriptorSetLayoutBinding{.binding = j,
                                                             .descriptorType = type,
                                                             .descriptorCount = 1,
                                                             .stageFlags = ToVkStageFlags(type)};

            update_entries[j] =
                vk::DescriptorUpdateTemplateEntry{.dstBinding = j,
                                                  .dstArrayElement = 0,
                                                  .descriptorCount = 1,
                                                  .descriptorType = type,
                                                  .offset = j * sizeof(DescriptorData),
                                                  .stride = 0};
        }

        const vk::DescriptorSetLayoutCreateInfo layout_info = {.bindingCount = set.binding_count,
                                                               .pBindings = set_bindings.data()};
        descriptor_set_layouts[i] = device.createDescriptorSetLayout(layout_info);

        const vk::DescriptorUpdateTemplateCreateInfo template_info = {
            .descriptorUpdateEntryCount = set.binding_count,
            .pDescriptorUpdateEntries = update_entries.data(),
            .templateType = vk::DescriptorUpdateTemplateType::eDescriptorSet,
            .descriptorSetLayout = descriptor_set_layouts[i]};

        update_templates[i] = device.createDescriptorUpdateTemplate(template_info);
    }

    const vk::PipelineLayoutCreateInfo layout_info = {.setLayoutCount = MAX_DESCRIPTOR_SETS,
                                                      .pSetLayouts = descriptor_set_layouts.data(),
                                                      .pushConstantRangeCount = 0,
                                                      .pPushConstantRanges = nullptr};

    pipeline_layout = device.createPipelineLayout(layout_info);
}

vk::DescriptorSet DescriptorManager::AllocateSet(vk::DescriptorSetLayout layout) {
    vk::Device device = instance.GetDevice();

    const vk::DescriptorSetAllocateInfo alloc_info = {
        .descriptorPool = current_pool, .descriptorSetCount = 1, .pSetLayouts = &layout};

    try {
        return device.allocateDescriptorSets(alloc_info)[0];
    } catch (vk::OutOfPoolMemoryError) {
        pool_provider.RefreshTick();
        current_pool = pool_provider.Commit();
    }

    return AllocateSet(layout);
}

} // namespace Vulkan
