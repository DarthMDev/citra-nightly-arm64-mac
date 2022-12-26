// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_stream_buffer.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

[[nodiscard]] vk::AccessFlags MakeAccessFlags(vk::BufferUsageFlagBits usage) {
    switch (usage) {
    case vk::BufferUsageFlagBits::eVertexBuffer:
        return vk::AccessFlagBits::eVertexAttributeRead;
    case vk::BufferUsageFlagBits::eIndexBuffer:
        return vk::AccessFlagBits::eIndexRead;
    case vk::BufferUsageFlagBits::eUniformBuffer:
        return vk::AccessFlagBits::eUniformRead;
    case vk::BufferUsageFlagBits::eUniformTexelBuffer:
        return vk::AccessFlagBits::eShaderRead;
    default:
        LOG_CRITICAL(Render_Vulkan, "Unknown usage flag {}", usage);
        UNREACHABLE();
    }
    return vk::AccessFlagBits::eNone;
}

[[nodiscard]] vk::PipelineStageFlags MakePipelineStage(vk::BufferUsageFlagBits usage) {
    switch (usage) {
    case vk::BufferUsageFlagBits::eVertexBuffer:
        return vk::PipelineStageFlagBits::eVertexInput;
    case vk::BufferUsageFlagBits::eIndexBuffer:
        return vk::PipelineStageFlagBits::eVertexInput;
    case vk::BufferUsageFlagBits::eUniformBuffer:
        return vk::PipelineStageFlagBits::eVertexShader |
               vk::PipelineStageFlagBits::eGeometryShader |
               vk::PipelineStageFlagBits::eFragmentShader;
    case vk::BufferUsageFlagBits::eUniformTexelBuffer:
        return vk::PipelineStageFlagBits::eFragmentShader;
    default:
        LOG_CRITICAL(Render_Vulkan, "Unknown usage flag {}", usage);
        UNREACHABLE();
    }
    return vk::PipelineStageFlagBits::eNone;
}

StagingBuffer::StagingBuffer(const Instance& instance, u32 size, bool readback)
    : instance{instance} {
    const vk::BufferUsageFlags usage =
        readback ? vk::BufferUsageFlagBits::eTransferDst : vk::BufferUsageFlagBits::eTransferSrc;
    const vk::BufferCreateInfo buffer_info = {.size = size, .usage = usage};

    const VmaAllocationCreateFlags flags =
        readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                 : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    const VmaAllocationCreateInfo alloc_create_info = {.flags =
                                                           flags | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                                       .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};

    VkBuffer unsafe_buffer = VK_NULL_HANDLE;
    VkBufferCreateInfo unsafe_buffer_info = static_cast<VkBufferCreateInfo>(buffer_info);
    VmaAllocationInfo alloc_info;
    VmaAllocator allocator = instance.GetAllocator();

    vmaCreateBuffer(allocator, &unsafe_buffer_info, &alloc_create_info, &unsafe_buffer, &allocation,
                    &alloc_info);

    buffer = vk::Buffer{unsafe_buffer};
    mapped = std::span{reinterpret_cast<std::byte*>(alloc_info.pMappedData), size};
}

StagingBuffer::~StagingBuffer() {
    vmaDestroyBuffer(instance.GetAllocator(), static_cast<VkBuffer>(buffer), allocation);
}

StreamBuffer::StreamBuffer(const Instance& instance, Scheduler& scheduler, u32 size, bool readback)
    : instance{instance}, scheduler{scheduler}, staging{instance, size, readback}, total_size{size},
      bucket_size{size / BUCKET_COUNT}, readback{readback} {}

StreamBuffer::StreamBuffer(const Instance& instance, Scheduler& scheduler, u32 size,
                           vk::BufferUsageFlagBits usage, std::span<const vk::Format> view_formats,
                           bool readback)
    : instance{instance}, scheduler{scheduler}, staging{instance, size, readback}, usage{usage},
      total_size{size}, bucket_size{size / BUCKET_COUNT}, readback{readback} {
    const vk::BufferCreateInfo buffer_info = {
        .size = total_size, .usage = usage | vk::BufferUsageFlagBits::eTransferDst};

    const VmaAllocationCreateInfo alloc_create_info = {.usage =
                                                           VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};

    VkBuffer unsafe_buffer = VK_NULL_HANDLE;
    VkBufferCreateInfo unsafe_buffer_info = static_cast<VkBufferCreateInfo>(buffer_info);
    VmaAllocationInfo alloc_info;
    VmaAllocator allocator = instance.GetAllocator();

    vmaCreateBuffer(allocator, &unsafe_buffer_info, &alloc_create_info, &unsafe_buffer, &allocation,
                    &alloc_info);

    gpu_buffer = vk::Buffer{unsafe_buffer};

    ASSERT(view_formats.size() < MAX_BUFFER_VIEWS);

    vk::Device device = instance.GetDevice();
    for (std::size_t i = 0; i < view_formats.size(); i++) {
        const vk::BufferViewCreateInfo view_info = {
            .buffer = gpu_buffer, .format = view_formats[i], .offset = 0, .range = total_size};

        views[i] = device.createBufferView(view_info);
    }

    view_count = view_formats.size();
}

StreamBuffer::~StreamBuffer() {
    if (gpu_buffer) {
        vk::Device device = instance.GetDevice();
        vmaDestroyBuffer(instance.GetAllocator(), static_cast<VkBuffer>(gpu_buffer), allocation);
        for (std::size_t i = 0; i < view_count; i++) {
            device.destroyBufferView(views[i]);
        }
    }
}

std::tuple<u8*, u32, bool> StreamBuffer::Map(u32 size) {
    ASSERT(size <= total_size);
    size = Common::AlignUp(size, 16);

    Bucket& bucket = buckets[bucket_index];

    // If we reach bucket boundaries move over to the next one
    if (bucket.cursor + size > bucket_size) {
        bucket.gpu_tick = scheduler.CurrentTick();
        MoveNextBucket();
        return Map(size);
    }

    const bool invalidate = std::exchange(bucket.invalid, false);
    const u32 buffer_offset = bucket_index * bucket_size + bucket.cursor;
    u8* mapped = reinterpret_cast<u8*>(staging.mapped.data() + buffer_offset);

    return std::make_tuple(mapped, buffer_offset, invalidate);
}

void StreamBuffer::Commit(u32 size) {
    size = Common::AlignUp(size, 16);
    buckets[bucket_index].cursor += size;
}

void StreamBuffer::Flush() {
    if (readback) {
        LOG_WARNING(Render_Vulkan, "Cannot flush read only buffer");
        return;
    }

    Bucket& bucket = buckets[bucket_index];
    const u32 flush_start = bucket_index * bucket_size + bucket.flush_cursor;
    const u32 flush_size = bucket.cursor - bucket.flush_cursor;
    ASSERT(flush_size <= bucket_size);
    ASSERT(flush_start + flush_size <= total_size);

    if (flush_size > 0) [[likely]] {
        // Ensure all staging writes are visible to the host memory domain
        VmaAllocator allocator = instance.GetAllocator();
        vmaFlushAllocation(allocator, staging.allocation, flush_start, flush_size);
        if (gpu_buffer) {
            scheduler.Record([this, flush_start, flush_size](vk::CommandBuffer,
                                                             vk::CommandBuffer upload_cmdbuf) {
                const vk::BufferCopy copy_region = {
                    .srcOffset = flush_start, .dstOffset = flush_start, .size = flush_size};

                upload_cmdbuf.copyBuffer(staging.buffer, gpu_buffer, copy_region);

                const vk::BufferMemoryBarrier buffer_barrier = {
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = MakeAccessFlags(usage),
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = gpu_buffer,
                    .offset = flush_start,
                    .size = flush_size};

                upload_cmdbuf.pipelineBarrier(
                    vk::PipelineStageFlagBits::eTransfer, MakePipelineStage(usage),
                    vk::DependencyFlagBits::eByRegion, {}, buffer_barrier, {});
            });
        }
        bucket.flush_cursor += flush_size;
    }
}

void StreamBuffer::Invalidate() {
    if (!readback) {
        return;
    }

    Bucket& bucket = buckets[bucket_index];
    const u32 flush_start = bucket_index * bucket_size + bucket.flush_cursor;
    const u32 flush_size = bucket.cursor - bucket.flush_cursor;
    ASSERT(flush_size <= bucket_size);

    if (flush_size > 0) [[likely]] {
        // Ensure the staging memory can be read by the host
        VmaAllocator allocator = instance.GetAllocator();
        vmaInvalidateAllocation(allocator, staging.allocation, flush_start, flush_size);
        bucket.flush_cursor += flush_size;
    }
}

void StreamBuffer::MoveNextBucket() {
    // Flush and Invalidate are bucket local operations for simplicity so perform them here
    if (readback) {
        Invalidate();
    } else {
        Flush();
    }

    bucket_index = (bucket_index + 1) % BUCKET_COUNT;
    Bucket& next_bucket = buckets[bucket_index];
    scheduler.Wait(next_bucket.gpu_tick);
    next_bucket.cursor = 0;
    next_bucket.flush_cursor = 0;
    next_bucket.invalid = true;
}

} // namespace Vulkan
