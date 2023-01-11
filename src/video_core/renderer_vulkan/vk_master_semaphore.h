// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <deque>
#include <limits>
#include <thread>
#include <vector>
#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;

constexpr u64 WAIT_TIMEOUT = std::numeric_limits<u64>::max();

class MasterSemaphore {
public:
    explicit MasterSemaphore(const Instance& instance);
    ~MasterSemaphore();

    /// Returns the current logical tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick.load(std::memory_order_acquire);
    }

    /// Returns the last known GPU tick.
    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return gpu_tick.load(std::memory_order_acquire);
    }

    /// Returns the timeline semaphore handle.
    [[nodiscard]] vk::Semaphore Handle() const noexcept {
        return semaphore;
    }

    /// Returns true when a tick has been hit by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return KnownGpuTick() >= tick;
    }

    /// Advance to the logical tick and return the old one
    [[nodiscard]] u64 NextTick() noexcept {
        return current_tick.fetch_add(1, std::memory_order_release);
    }

    /// Refresh the known GPU tick
    void Refresh() {
        u64 this_tick{};
        u64 counter{};
        do {
            this_tick = gpu_tick.load(std::memory_order_acquire);
            counter = device.getSemaphoreCounterValueKHR(semaphore);
            if (counter < this_tick) {
                return;
            }
        } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                                 std::memory_order_relaxed));
    }

    /// Waits for a tick to be hit on the GPU
    void Wait(u64 tick) {
        // No need to wait if the GPU is ahead of the tick
        if (IsFree(tick)) {
            return;
        }
        // Update the GPU tick and try again
        Refresh();
        if (IsFree(tick)) {
            return;
        }

        // If none of the above is hit, fallback to a regular wait
        const vk::SemaphoreWaitInfoKHR wait_info = {
            .semaphoreCount = 1,
            .pSemaphores = &semaphore,
            .pValues = &tick,
        };

        while (device.waitSemaphoresKHR(&wait_info, WAIT_TIMEOUT) != vk::Result::eSuccess) {
        }
        Refresh();
    }

private:
    vk::Device device;
    vk::Semaphore semaphore;          ///< Timeline semaphore.
    std::atomic<u64> gpu_tick{0};     ///< Current known GPU tick.
    std::atomic<u64> current_tick{1}; ///< Current logical tick.
};

class MasterSemaphoreFence {
    static constexpr std::size_t FENCE_RESERVE_COUNT = 8;
public:
    explicit MasterSemaphoreFence(const Instance& instance);
    ~MasterSemaphoreFence();

    /// Returns the current logical tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick.load(std::memory_order_acquire);
    }

    /// Returns the last known GPU tick.
    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return gpu_tick.load(std::memory_order_acquire);
    }

    /// Attempts to retrieve a reserved fence
    [[nodiscard]] vk::Fence TryGetReservedFence() {
        if (fence_reserve.empty()) {
            return VK_NULL_HANDLE;
        }

        vk::Fence fence = fence_reserve.back();
        fence_reserve.pop_back();
        return fence;
    }

    /// Returns an available fence for queue submission.
    [[nodiscard]] vk::Fence Handle() noexcept {
        vk::Fence fence{};
        if (fence = TryGetReservedFence(); !fence) {
            fence = device.createFence({});
        }

        const u64 tick{CurrentTick()};
        fences.push_front(Fence{
            .fence = fence,
            .gpu_tick = tick
        });
        return fence;
    }

    /// Returns true when a tick has been hit by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return KnownGpuTick() >= tick;
    }

    /// Advance to the logical tick and return the old one
    [[nodiscard]] u64 NextTick() noexcept {
        return current_tick.fetch_add(1, std::memory_order_release);
    }

    /// Returns the tick of the most recent signaled fence
    [[nodiscard]] u64 FenceCounterValue() {
        if (fences.empty()) [[unlikely]] {
            return CurrentTick();
        }

        u64 tick{KnownGpuTick()};
        do {
            Fence fence = fences.back();
            if (device.getFenceStatus(fence.fence) != vk::Result::eSuccess) {
                return tick;
            }
            tick = fence.gpu_tick;
            device.resetFences(fence.fence);
            fence_reserve.push_back(fence.fence);
            fences.pop_back();
        } while (!fences.empty());

        return tick;
    }

    /// Refresh the known GPU tick
    void Refresh() {
        u64 this_tick{};
        u64 counter{};
        do {
            this_tick = gpu_tick.load(std::memory_order_acquire);
            counter = FenceCounterValue();
            if (counter < this_tick) {
                return;
            }
        } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                                 std::memory_order_relaxed));
    }

    /// Waits for a tick to be hit on the GPU
    void Wait(u64 tick) {
        // No need to wait if the GPU is ahead of the tick
        if (IsFree(tick)) {
            return;
        }
        // Update the GPU tick and try again
        Refresh();
        if (IsFree(tick)) {
            return;
        }

        // If none of the above is hit, search for the fence
        // with the requested tick and wait for it
        for (const Fence& fence : fences) {
            if (fence.gpu_tick == tick) {
                void(device.waitForFences(fence.fence, true, WAIT_TIMEOUT));
                Refresh();
                return;
            }
        }

        UNREACHABLE_MSG("Attempting to wait for tick {} that has not been submitted", tick);
    }

private:
    vk::Device device;
    std::atomic<u64> gpu_tick{0};     ///< Current known GPU tick.
    std::atomic<u64> current_tick{1}; ///< Current logical tick.

    struct Fence {
        vk::Fence fence;
        u64 gpu_tick;
    };
    std::deque<Fence> fences; ///< List of pending fences
    std::vector<vk::Fence> fence_reserve; ///< Reserve of unsignaled fences
};

} // namespace Vulkan
