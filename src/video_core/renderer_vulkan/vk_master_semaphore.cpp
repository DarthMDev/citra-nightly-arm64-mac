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

} // namespace Vulkan
