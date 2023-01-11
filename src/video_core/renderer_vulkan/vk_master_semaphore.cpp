// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"

namespace Vulkan {

MasterSemaphore::MasterSemaphore(const Instance& instance) : device{instance.GetDevice()} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfoKHR{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    semaphore = device.createSemaphore(semaphore_chain.get());
}

MasterSemaphore::~MasterSemaphore() {
    device.destroySemaphore(semaphore);
}

MasterSemaphoreFence::MasterSemaphoreFence(const Instance& instance) : device{instance.GetDevice()} {
    fence_reserve.resize(FENCE_RESERVE_COUNT);
    for (vk::Fence& fence : fence_reserve) {
        fence = device.createFence({});
    }
}

MasterSemaphoreFence::~MasterSemaphoreFence() {
    device.waitIdle();
    for (const vk::Fence fence : fence_reserve) {
        device.destroyFence(fence);
    }
    for (const Fence& fence : fences) {
        device.destroyFence(fence.fence);
    }
}

} // namespace Vulkan
