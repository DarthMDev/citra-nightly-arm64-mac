// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <limits>
#include "common/alignment.h"
#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_stream_buffer.h"

namespace Vulkan {

namespace {

constexpr u64 WATCHES_INITIAL_RESERVE = 0x4000;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x1000;

/// Find a memory type with the passed requirements
std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted,
                                  u32 filter = std::numeric_limits<u32>::max()) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted && (filter & (1U << i)) != 0) {
            return i;
        }
    }
    return std::nullopt;
}

/// Get the preferred host visible memory type.
u32 GetMemoryType(const vk::PhysicalDeviceMemoryProperties& properties, bool readback,
                  u32 filter = std::numeric_limits<u32>::max()) {
    // Prefer device local host visible allocations. Both AMD and Nvidia now provide one.
    // Otherwise search for a host visible allocation.
    const vk::MemoryPropertyFlags HOST_MEMORY =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    const vk::MemoryPropertyFlags DYNAMIC_MEMORY =
        HOST_MEMORY | (readback ? vk::MemoryPropertyFlagBits::eHostCached
                                : vk::MemoryPropertyFlagBits::eDeviceLocal);

    std::optional preferred_type = FindMemoryType(properties, DYNAMIC_MEMORY);
    if (!preferred_type) {
        preferred_type = FindMemoryType(properties, HOST_MEMORY);
        ASSERT_MSG(preferred_type, "No host visible and coherent memory type found");
    }
    return preferred_type.value_or(0);
}

} // Anonymous namespace

StreamBuffer::StreamBuffer(const Instance& instance_, Scheduler& scheduler_,
                           vk::BufferUsageFlags usage_, u64 size, bool readback_)
    : instance{instance_}, scheduler{scheduler_}, usage{usage_}, readback{readback_} {
    CreateBuffers(size);
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
}

StreamBuffer::~StreamBuffer() {
    const vk::Device device = instance.GetDevice();
    device.unmapMemory(memory);
    device.destroyBuffer(buffer);
    device.freeMemory(memory);
}

std::tuple<u8*, u64, bool> StreamBuffer::Map(u64 size, u64 alignment) {
    ASSERT(size <= stream_buffer_size);
    mapped_size = size;

    if (alignment > 0) {
        offset = Common::AlignUp(offset, alignment);
    }

    WaitPendingOperations(offset);

    bool invalidate{false};
    if (offset + size > stream_buffer_size) {
        // The buffer would overflow, save the amount of used watches and reset the state.
        invalidate = true;
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;

        // Swap watches and reset waiting cursors.
        std::swap(previous_watches, current_watches);
        wait_cursor = 0;
        wait_bound = 0;
    }

    return std::make_tuple(mapped + offset, offset, invalidate);
}

void StreamBuffer::Commit(u64 size) {
    ASSERT_MSG(size <= mapped_size, "Reserved size {} is too small compared to {}", mapped_size,
               size);

    offset += size;

    if (current_watch_cursor + 1 >= current_watches.size()) {
        // Ensure that there are enough watches.
        ReserveWatches(current_watches, WATCHES_RESERVE_CHUNK);
    }
    auto& watch = current_watches[current_watch_cursor++];
    watch.upper_bound = offset;
    watch.tick = scheduler.CurrentTick();
}

void StreamBuffer::CreateBuffers(u64 prefered_size) {
    const vk::Device device = instance.GetDevice();
    const auto memory_properties = instance.GetPhysicalDevice().getMemoryProperties();
    const u32 preferred_type = GetMemoryType(memory_properties, readback);
    const u32 preferred_heap = memory_properties.memoryTypes[preferred_type].heapIndex;

    // Substract from the preferred heap size some bytes to avoid getting out of memory.
    const VkDeviceSize heap_size = memory_properties.memoryHeaps[preferred_heap].size;
    // As per DXVK's example, using `heap_size / 2`
    const VkDeviceSize allocable_size = heap_size / 2;
    buffer = device.createBuffer({
        .size = std::min(prefered_size, allocable_size),
        .usage = usage,
    });

    const auto requirements = device.getBufferMemoryRequirements(buffer);
    const u32 required_flags = requirements.memoryTypeBits;
    stream_buffer_size = static_cast<u64>(requirements.size);

    memory = device.allocateMemory({
        .allocationSize = requirements.size,
        .memoryTypeIndex = GetMemoryType(memory_properties, required_flags),
    });

    device.bindBufferMemory(buffer, memory, 0);
    mapped = reinterpret_cast<u8*>(device.mapMemory(memory, 0, VK_WHOLE_SIZE));
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
}

void StreamBuffer::WaitPendingOperations(u64 requested_upper_bound) {
    if (!invalidation_mark) {
        return;
    }
    while (requested_upper_bound < wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        wait_bound = watch.upper_bound;
        scheduler.Wait(watch.tick);
        ++wait_cursor;
    }
}

} // namespace Vulkan
