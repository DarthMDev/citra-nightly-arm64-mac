// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <utility>
#include "common/microprofile.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

void Scheduler::CommandChunk::ExecuteAll(vk::CommandBuffer render_cmdbuf,
                                         vk::CommandBuffer upload_cmdbuf) {
    auto command = first;
    while (command != nullptr) {
        auto next = command->GetNext();
        command->Execute(render_cmdbuf, upload_cmdbuf);
        command->~Command();
        command = next;
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

Scheduler::Scheduler(const Instance& instance, RenderpassCache& renderpass_cache,
                     RendererVulkan& renderer)
    : instance{instance}, renderpass_cache{renderpass_cache}, renderer{renderer},
      master_semaphore{instance}, command_pool{instance, master_semaphore}, stop_requested{false},
      use_worker_thread{Settings::values.async_command_recording} {
    AllocateWorkerCommandBuffers();
    if (use_worker_thread) {
        AcquireNewChunk();
        worker_thread = std::thread([this]() { WorkerThread(); });
    }
}

Scheduler::~Scheduler() {
    if (!use_worker_thread) {
        return;
    }

    stop_requested = true;

    // Push a dummy chunk to unblock the thread
    {
        std::scoped_lock lock{work_mutex};
        work_queue.push(std::move(chunk));
    }

    work_cv.notify_one();
    worker_thread.join();
}

void Scheduler::Flush(vk::Semaphore signal, vk::Semaphore wait) {
    SubmitExecution(signal, wait);
}

void Scheduler::Finish(vk::Semaphore signal, vk::Semaphore wait) {
    const u64 presubmit_tick = CurrentTick();
    SubmitExecution(signal, wait);
    WaitWorker();
    Wait(presubmit_tick);
}

MICROPROFILE_DEFINE(Vulkan_WaitForWorker, "Vulkan", "Wait for worker", MP_RGB(255, 192, 192));
void Scheduler::WaitWorker() {
    if (!use_worker_thread) {
        return;
    }

    MICROPROFILE_SCOPE(Vulkan_WaitForWorker);
    DispatchWork();

    std::unique_lock lock{work_mutex};
    wait_cv.wait(lock, [this] { return work_queue.empty(); });
}

void Scheduler::DispatchWork() {
    if (chunk->Empty()) {
        return;
    }

    {
        std::scoped_lock lock{work_mutex};
        work_queue.push(std::move(chunk));
    }

    work_cv.notify_one();
    AcquireNewChunk();
}

void Scheduler::WorkerThread() {
    do {
        std::unique_ptr<CommandChunk> work;
        bool has_submit{false};
        {
            std::unique_lock lock{work_mutex};
            if (work_queue.empty()) {
                wait_cv.notify_all();
            }
            work_cv.wait(lock, [this] { return !work_queue.empty() || stop_requested; });
            if (stop_requested) {
                continue;
            }
            work = std::move(work_queue.front());
            work_queue.pop();

            has_submit = work->HasSubmit();
            work->ExecuteAll(render_cmdbuf, upload_cmdbuf);
        }
        if (has_submit) {
            AllocateWorkerCommandBuffers();
        }
        std::scoped_lock reserve_lock{reserve_mutex};
        chunk_reserve.push_back(std::move(work));
    } while (!stop_requested);
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};

    upload_cmdbuf = command_pool.Commit();
    upload_cmdbuf.begin(begin_info);

    render_cmdbuf = command_pool.Commit();
    render_cmdbuf.begin(begin_info);
}

MICROPROFILE_DEFINE(Vulkan_Submit, "Vulkan", "Submit Exectution", MP_RGB(255, 192, 255));
void Scheduler::SubmitExecution(vk::Semaphore signal_semaphore, vk::Semaphore wait_semaphore) {
    renderer.FlushBuffers();
    const u64 signal_value = master_semaphore.NextTick();
    state = StateFlags::AllDirty;

    renderpass_cache.ExitRenderpass();
    Record([signal_semaphore, wait_semaphore, signal_value, this](vk::CommandBuffer render_cmdbuf,
                                                                  vk::CommandBuffer upload_cmdbuf) {
        MICROPROFILE_SCOPE(Vulkan_Submit);
        upload_cmdbuf.end();
        render_cmdbuf.end();

        const vk::Semaphore timeline_semaphore = master_semaphore.Handle();

        const u32 num_signal_semaphores = signal_semaphore ? 2U : 1U;
        const std::array signal_values{signal_value, u64(0)};
        const std::array signal_semaphores{timeline_semaphore, signal_semaphore};

        const u32 num_wait_semaphores = wait_semaphore ? 2U : 1U;
        const std::array wait_values{signal_value - 1, u64(1)};
        const std::array wait_semaphores{timeline_semaphore, wait_semaphore};

        static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
            vk::PipelineStageFlagBits::eAllCommands,
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
        };

        const vk::TimelineSemaphoreSubmitInfoKHR timeline_si = {
            .waitSemaphoreValueCount = num_wait_semaphores,
            .pWaitSemaphoreValues = wait_values.data(),
            .signalSemaphoreValueCount = num_signal_semaphores,
            .pSignalSemaphoreValues = signal_values.data(),
        };

        const std::array cmdbuffers = {upload_cmdbuf, render_cmdbuf};
        const vk::SubmitInfo submit_info = {
            .pNext = &timeline_si,
            .waitSemaphoreCount = num_wait_semaphores,
            .pWaitSemaphores = wait_semaphores.data(),
            .pWaitDstStageMask = wait_stage_masks.data(),
            .commandBufferCount = static_cast<u32>(cmdbuffers.size()),
            .pCommandBuffers = cmdbuffers.data(),
            .signalSemaphoreCount = num_signal_semaphores,
            .pSignalSemaphores = signal_semaphores.data(),
        };

        try {
            vk::Queue queue = instance.GetGraphicsQueue();
            queue.submit(submit_info);
        } catch (vk::DeviceLostError& err) {
            LOG_CRITICAL(Render_Vulkan, "Device lost during submit: {}", err.what());
            UNREACHABLE();
        }
    });

    if (!use_worker_thread) {
        AllocateWorkerCommandBuffers();
    } else {
        chunk->MarkSubmit();
        DispatchWork();
    }
}

void Scheduler::AcquireNewChunk() {
    std::scoped_lock lock{reserve_mutex};
    if (chunk_reserve.empty()) {
        chunk = std::make_unique<CommandChunk>();
        return;
    }

    chunk = std::move(chunk_reserve.back());
    chunk_reserve.pop_back();
}

} // namespace Vulkan
