// Copyright 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "VirtioGpuTimelines.h"

#include <cinttypes>
#include <cstdio>

#include "gfxstream/host/Tracing.h"
#include "host-common/GfxstreamFatalError.h"

using TaskId = VirtioGpuTimelines::TaskId;
using Ring = VirtioGpuTimelines::Ring;
using FenceId = VirtioGpuTimelines::FenceId;
using AutoLock = android::base::AutoLock;
using emugl::ABORT_REASON_OTHER;
using emugl::FatalError;

std::unique_ptr<VirtioGpuTimelines> VirtioGpuTimelines::create(bool withAsyncCallback) {
    return std::unique_ptr<VirtioGpuTimelines>(new VirtioGpuTimelines(withAsyncCallback));
}

VirtioGpuTimelines::VirtioGpuTimelines(bool withAsyncCallback)
    : mNextId(0), mWithAsyncCallback(withAsyncCallback) {}

TaskId VirtioGpuTimelines::enqueueTask(const Ring& ring) {
    AutoLock lock(mLock);

    TaskId id = mNextId++;

    const uint64_t traceId = gfxstream::host::GetUniqueTracingId();
    GFXSTREAM_TRACE_EVENT_INSTANT(GFXSTREAM_TRACE_VIRTIO_GPU_TIMELINE_CATEGORY,
                                  "Queue timeline task", "Task ID", id,
                                  GFXSTREAM_TRACE_FLOW(traceId));

    std::shared_ptr<Task> task(new Task(id, ring, traceId), [this](Task* task) {
        mTaskIdToTask.erase(task->mId);
        delete task;
    });
    mTaskIdToTask[id] = task;

    Timeline& timeline = GetOrCreateTimelineLocked(ring);
    timeline.mQueue.emplace_back(std::move(task));
    return id;
}

void VirtioGpuTimelines::enqueueFence(const Ring& ring, FenceId fenceId,
                                      FenceCompletionCallback fenceCompletionCallback) {
    AutoLock lock(mLock);

    auto fence = std::make_unique<Fence>(fenceId, std::move(fenceCompletionCallback));

    Timeline& timeline = GetOrCreateTimelineLocked(ring);
    timeline.mQueue.emplace_back(std::move(fence));
    if (mWithAsyncCallback) {
        poll_locked(ring);
    }
}

void VirtioGpuTimelines::notifyTaskCompletion(TaskId taskId) {
    AutoLock lock(mLock);
    auto iTask = mTaskIdToTask.find(taskId);
    if (iTask == mTaskIdToTask.end()) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Task(id = " << static_cast<uint64_t>(taskId) << ") can't be found";
    }
    std::shared_ptr<Task> task = iTask->second.lock();
    if (task == nullptr) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Task(id = " << static_cast<uint64_t>(taskId) << ") has been destroyed";
    }
    if (task->mId != taskId) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Task id mismatch. Expected " << static_cast<uint64_t>(taskId) << " Actual "
            << static_cast<uint64_t>(task->mId);
    }
    if (task->mHasCompleted) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Task(id = " << static_cast<uint64_t>(taskId) << ") has been set to completed.";
    }

    GFXSTREAM_TRACE_EVENT_INSTANT(GFXSTREAM_TRACE_VIRTIO_GPU_TIMELINE_CATEGORY,
                                  "Notify timeline task completed",
                                  GFXSTREAM_TRACE_FLOW(task->mTraceId), "Task ID", task->mId);

    task->mHasCompleted = true;
    if (mWithAsyncCallback) {
        poll_locked(task->mRing);
    }
}

VirtioGpuTimelines::Timeline& VirtioGpuTimelines::GetOrCreateTimelineLocked(const Ring& ring) {
    auto [it, inserted] =
        mTimelineQueues.emplace(std::piecewise_construct, std::make_tuple(ring), std::make_tuple());
    Timeline& timeline = it->second;
    if (inserted) {
        timeline.mTraceTrackId = gfxstream::host::GetUniqueTracingId();

        const std::string timelineName = "Virtio Gpu Timeline " + to_string(ring);
        GFXSTREAM_TRACE_NAME_TRACK(GFXSTREAM_TRACE_TRACK(timeline.mTraceTrackId), timelineName);

        GFXSTREAM_TRACE_EVENT_INSTANT(GFXSTREAM_TRACE_VIRTIO_GPU_TIMELINE_CATEGORY,
                                      "Create Timeline",
                                      GFXSTREAM_TRACE_TRACK(timeline.mTraceTrackId));
    }

    return timeline;
}

void VirtioGpuTimelines::poll() {
    if (mWithAsyncCallback) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Can't call poll with async callback enabled.";
    }
    AutoLock lock(mLock);
    for (const auto& [ring, timeline] : mTimelineQueues) {
        poll_locked(ring);
    }
}
void VirtioGpuTimelines::poll_locked(const Ring& ring) {
    auto timelineIt = mTimelineQueues.find(ring);
    if (timelineIt == mTimelineQueues.end()) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Ring(" << to_string(ring) << ") doesn't exist.";
    }
    Timeline& timeline = timelineIt->second;

    auto& timelineQueue = timeline.mQueue;
    auto i = timelineQueue.begin();
    for (; i != timelineQueue.end(); i++) {
        bool shouldStop = std::visit(
            [&](auto& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<Fence>>) {
                    auto& fence = arg;

                    GFXSTREAM_TRACE_EVENT_INSTANT(
                        GFXSTREAM_TRACE_VIRTIO_GPU_TIMELINE_CATEGORY, "Signal Virtio Gpu Fence",
                        GFXSTREAM_TRACE_TRACK(timeline.mTraceTrackId), "Fence", fence->mId);

                    fence->mCompletionCallback();

                    return false;
                } else if constexpr (std::is_same_v<T, std::shared_ptr<Task>>) {
                    auto& task = arg;

                    const bool completed = task->mHasCompleted;
                    if (completed) {
                        GFXSTREAM_TRACE_EVENT_INSTANT(
                            GFXSTREAM_TRACE_VIRTIO_GPU_TIMELINE_CATEGORY, "Process Task Complete",
                            GFXSTREAM_TRACE_TRACK(timeline.mTraceTrackId),
                            GFXSTREAM_TRACE_FLOW(task->mTraceId), "Task", task->mId);
                    }
                    return !completed;
                }
            },
            *i);

        if (shouldStop) {
            break;
        }
    }
    timelineQueue.erase(timelineQueue.begin(), i);
}
