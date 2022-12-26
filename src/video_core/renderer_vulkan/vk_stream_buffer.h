// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <map>
#include <span>
#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_common.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {

class Instance;
class Scheduler;

struct StagingBuffer {
    StagingBuffer(const Instance& instance, u32 size, bool readback);
    ~StagingBuffer();

    const Instance& instance;
    vk::Buffer buffer{};
    VmaAllocation allocation{};
    std::span<std::byte> mapped{};
};

class StreamBuffer {
    static constexpr u32 MAX_BUFFER_VIEWS = 3;
    static constexpr u32 BUCKET_COUNT = 4;

public:
    /// Staging only constructor
    StreamBuffer(const Instance& instance, Scheduler& scheduler, u32 size, bool readback = false);
    /// Staging + GPU streaming constructor
    StreamBuffer(const Instance& instance, Scheduler& scheduler, u32 size,
                 vk::BufferUsageFlagBits usage, std::span<const vk::Format> views,
                 bool readback = false);
    ~StreamBuffer();

    StreamBuffer(const StreamBuffer&) = delete;
    StreamBuffer& operator=(const StreamBuffer&) = delete;

    /// Maps aligned staging memory of size bytes
    std::tuple<u8*, u32, bool> Map(u32 size);

    /// Commits size bytes from the currently mapped staging memory
    void Commit(u32 size = 0);

    /// Flushes staging memory to the GPU buffer
    void Flush();

    /// Invalidates staging memory for reading
    void Invalidate();

    /// Returns the GPU buffer handle
    [[nodiscard]] vk::Buffer GetHandle() const {
        return gpu_buffer;
    }

    /// Returns the staging buffer handle
    [[nodiscard]] vk::Buffer GetStagingHandle() const {
        return staging.buffer;
    }

    /// Returns an immutable reference to the requested buffer view
    [[nodiscard]] const vk::BufferView& GetView(u32 index = 0) const {
        ASSERT(index < view_count);
        return views[index];
    }

private:
    /// Moves to the next bucket
    void MoveNextBucket();

    struct Bucket {
        bool invalid = false;
        u32 gpu_tick = 0;
        u32 cursor = 0;
        u32 flush_cursor = 0;
    };

private:
    const Instance& instance;
    Scheduler& scheduler;
    StagingBuffer staging;
    vk::Buffer gpu_buffer{};
    VmaAllocation allocation{};
    vk::BufferUsageFlagBits usage;
    std::array<vk::BufferView, MAX_BUFFER_VIEWS> views{};
    std::array<Bucket, BUCKET_COUNT> buckets;
    std::size_t view_count = 0;
    u32 total_size = 0;
    u32 bucket_size = 0;
    u32 bucket_index = 0;
    bool readback = false;
};

} // namespace Vulkan
