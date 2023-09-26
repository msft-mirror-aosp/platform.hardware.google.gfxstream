// Copyright (C) 2023 The Android Open Source Project
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

#include "GfxstreamEnd2EndTests.h"

#include <filesystem>

#include <dlfcn.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <log/log.h>

#include "aemu/base/system/System.h"
#include "Gralloc.h"
#include "host-common/GfxstreamFatalError.h"
#include "host-common/logging.h"
#include "ProcessPipe.h"
#include "virgl_hw.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace gfxstream {
namespace tests {

using emugl::ABORT_REASON_OTHER;
using emugl::FatalError;
using testing::AnyOf;
using testing::Eq;
using testing::Gt;
using testing::IsTrue;
using testing::Not;
using testing::NotNull;

// For now, single guest process testing.
constexpr const uint32_t kVirtioGpuContextId = 1;

std::optional<uint32_t> GlFormatToDrmFormat(uint32_t glFormat) {
    switch (glFormat) {
        case GL_RGB565:
            return DRM_FORMAT_BGR565;
        case GL_RGBA:
            return DRM_FORMAT_ABGR8888;
    }
    return std::nullopt;
}

std::optional<uint32_t> DrmFormatToVirglFormat(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_BGR888:
        case DRM_FORMAT_RGB888:
            return VIRGL_FORMAT_R8G8B8_UNORM;
        case DRM_FORMAT_XRGB8888:
            return VIRGL_FORMAT_B8G8R8X8_UNORM;
        case DRM_FORMAT_ARGB8888:
            return VIRGL_FORMAT_B8G8R8A8_UNORM;
        case DRM_FORMAT_XBGR8888:
            return VIRGL_FORMAT_R8G8B8X8_UNORM;
        case DRM_FORMAT_ABGR8888:
            return VIRGL_FORMAT_R8G8B8A8_UNORM;
        case DRM_FORMAT_ABGR2101010:
            return VIRGL_FORMAT_R10G10B10A2_UNORM;
        case DRM_FORMAT_BGR565:
            return VIRGL_FORMAT_B5G6R5_UNORM;
        case DRM_FORMAT_R8:
            return VIRGL_FORMAT_R8_UNORM;
        case DRM_FORMAT_R16:
            return VIRGL_FORMAT_R16_UNORM;
        case DRM_FORMAT_RG88:
            return VIRGL_FORMAT_R8G8_UNORM;
        case DRM_FORMAT_NV12:
            return VIRGL_FORMAT_NV12;
        case DRM_FORMAT_NV21:
            return VIRGL_FORMAT_NV21;
        case DRM_FORMAT_YVU420:
            return VIRGL_FORMAT_YV12;
    }
    return std::nullopt;
}

TestingVirtGpuBlobMapping::TestingVirtGpuBlobMapping(VirtGpuBlobPtr blob, uint8_t* mapped)
    : mBlob(blob),
      mMapped(mapped) {}

TestingVirtGpuBlobMapping::~TestingVirtGpuBlobMapping(void) {
    stream_renderer_resource_unmap(mBlob->getResourceHandle());
}

uint8_t* TestingVirtGpuBlobMapping::asRawPtr(void) { return mMapped; }

TestingVirtGpuResource::TestingVirtGpuResource(
        uint32_t resourceId,
        ResourceType resourceType,
        std::shared_ptr<TestingVirtGpuDevice> device,
        std::shared_future<void> createCompleted,
        std::unique_ptr<uint8_t[]> resourceGuestBytes,
        std::shared_future<uint8_t*> mapCompleted)
    : mResourceId(resourceId),
        mResourceType(resourceType),
        mDevice(device),
        mPendingCommandWaitables({createCompleted}),
        mResourceGuestBytes(std::move(resourceGuestBytes)),
        mResourceMappedHostBytes(std::move(mapCompleted)) {}

/*static*/
std::shared_ptr<TestingVirtGpuResource> TestingVirtGpuResource::createBlob(
        uint32_t resourceId,
        std::shared_ptr<TestingVirtGpuDevice> device,
        std::shared_future<void> createCompleted,
        std::shared_future<uint8_t*> mapCompleted) {
    return std::shared_ptr<TestingVirtGpuResource>(
        new TestingVirtGpuResource(resourceId,
                                   ResourceType::kBlob,
                                   device,
                                   createCompleted,
                                   nullptr,
                                   mapCompleted));
}

/*static*/
std::shared_ptr<TestingVirtGpuResource> TestingVirtGpuResource::createPipe(
        uint32_t resourceId,
    std::shared_ptr<TestingVirtGpuDevice> device,
    std::shared_future<void> createCompleted,
    std::unique_ptr<uint8_t[]> resourceBytes) {
    return std::shared_ptr<TestingVirtGpuResource>(
        new TestingVirtGpuResource(resourceId,
                                   ResourceType::kPipe,
                                   device,
                                   createCompleted,
                                   std::move(resourceBytes)));
}

TestingVirtGpuResource::~TestingVirtGpuResource() {
    ALOGV("Unref resource:%d", (int)mResourceId);
    stream_renderer_resource_unref(mResourceId);
}

VirtGpuBlobMappingPtr TestingVirtGpuResource::createMapping(void) {
    uint8_t* mappedMemory = nullptr;

    if (mResourceType == ResourceType::kBlob) {
        if (!mResourceMappedHostBytes.valid()) {
            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                << "Attempting to map blob resource:"
                << mResourceId
                << " which was created without the mappable flag.";
        }

        mappedMemory = mResourceMappedHostBytes.get();
    } else if (mResourceType == ResourceType::kPipe) {
        mappedMemory = mResourceGuestBytes.get();
    } else {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Unhandled.";
    }

    return std::make_shared<TestingVirtGpuBlobMapping>(shared_from_this(), mappedMemory);
}

uint32_t TestingVirtGpuResource::getResourceHandle() {
    return mResourceId;
}

uint32_t TestingVirtGpuResource::getBlobHandle() {
    if (mResourceType != ResourceType::kBlob) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Attempting to get blob handle for non-blob resource";
    }

    GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Unimplemented";
    return 0;
}

int TestingVirtGpuResource::exportBlob(VirtGpuExternalHandle& handle) {
    if (mResourceType != ResourceType::kBlob) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Attempting to export blob for non-blob resource";
    }

    GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Unimplemented";
    return 0;
}

int TestingVirtGpuResource::wait() {
    std::vector<std::shared_future<void>> currentPendingCommandWaitables;

    {
        std::lock_guard<std::mutex> lock(mPendingCommandWaitablesMutex);
        currentPendingCommandWaitables = mPendingCommandWaitables;
        mPendingCommandWaitables.clear();
    }

    for (auto& waitable : currentPendingCommandWaitables) {
        waitable.wait();
    }

    return 0;
}

void TestingVirtGpuResource::addPendingCommandWaitable(std::shared_future<void> waitable) {
    std::lock_guard<std::mutex> lock(mPendingCommandWaitablesMutex);

    mPendingCommandWaitables.erase(
        std::remove_if(mPendingCommandWaitables.begin(),
                        mPendingCommandWaitables.end(),
                        [](const std::shared_future<void>& waitable) {
                            return waitable.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                        }),
        mPendingCommandWaitables.end());

    mPendingCommandWaitables.push_back(std::move(waitable));
}

int TestingVirtGpuResource::transferFromHost(uint32_t offset, uint32_t size) {
    if (mResourceType != ResourceType::kPipe) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Unexpected transferFromHost() called on non-pipe resource.";
    }

    std::shared_future<void> transferCompleteWaitable = mDevice->transferFromHost(mResourceId, offset, size);

    {
        std::lock_guard<std::mutex> lock(mPendingCommandWaitablesMutex);
        mPendingCommandWaitables.push_back(transferCompleteWaitable);
    }

    return 0;
}

int TestingVirtGpuResource::transferToHost(uint32_t offset, uint32_t size) {
    if (mResourceType != ResourceType::kPipe) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Unexpected transferFromHost() called on non-pipe resource.";
    }

    std::shared_future<void> transferCompleteWaitable = mDevice->transferToHost(mResourceId, offset, size);

    {
        std::lock_guard<std::mutex> lock(mPendingCommandWaitablesMutex);
        mPendingCommandWaitables.push_back(transferCompleteWaitable);
    }

    return 0;
}

TestingVirtGpuDevice::TestingVirtGpuDevice()
    : mVirtioGpuTaskProcessingThread([this]() { RunVirtioGpuTaskProcessingLoop(); }) {}

TestingVirtGpuDevice::~TestingVirtGpuDevice() {
    mShuttingDown = true;
    mVirtioGpuTaskProcessingThread.join();
}

int64_t TestingVirtGpuDevice::getDeviceHandle() { return -1; }

VirtGpuCaps TestingVirtGpuDevice::getCaps() {
    VirtGpuCaps caps = {
        .params =
            {
                [kParam3D] = 1,
                [kParamCapsetFix] = 1,
                [kParamResourceBlob] = 1,
                [kParamHostVisible] = 1,
                [kParamCrossDevice] = 0,
                [kParamContextInit] = 1,
                [kParamSupportedCapsetIds] = 0,
                [kParamCreateGuestHandle] = 0,
            },
    };

    stream_renderer_fill_caps(0, 0, &caps.vulkanCapset);
    return caps;
}

VirtGpuBlobPtr TestingVirtGpuDevice::createBlob(const struct VirtGpuCreateBlob& blobCreate) {
    const uint32_t resourceId = mNextVirtioGpuResourceId++;

    ALOGV("Enquing task to create blob resource-id:%d size:%" PRIu64, resourceId, blobCreate.size);

    VirtioGpuTaskCreateBlob createTask{
        .resourceId = resourceId,
        .params = {
            .blob_mem = static_cast<uint32_t>(blobCreate.blobMem),
            .blob_flags = static_cast<uint32_t>(blobCreate.flags),
            .blob_id = blobCreate.blobId,
            .size = blobCreate.size,
        },
    };
    auto createBlobCompletedWaitable = EnqueueVirtioGpuTask(std::move(createTask));

    std::shared_future<uint8_t*> mappedBytesWaitable;

    if (blobCreate.flags & kBlobFlagMappable) {
        std::promise<uint8_t*> mappedBytesPromise;
        mappedBytesWaitable = mappedBytesPromise.get_future();

        VirtioGpuTaskMap mapTask{
            .resourceId = resourceId,
            .resourceMappedPromise = std::move(mappedBytesPromise),
        };
        EnqueueVirtioGpuTask(std::move(mapTask));
    }

    return TestingVirtGpuResource::createBlob(resourceId, shared_from_this(), createBlobCompletedWaitable, std::move(mappedBytesWaitable));
}

VirtGpuBlobPtr TestingVirtGpuDevice::createPipeBlob(uint32_t size) {
    const uint32_t resourceId = mNextVirtioGpuResourceId++;

    auto resourceBytes = std::make_unique<uint8_t[]>(size);

    VirtioGpuTaskCreateResource task{
        .resourceId = resourceId,
        .resourceBytes = resourceBytes.get(),
        .params = {
            .handle = resourceId,
            .target = /*PIPE_BUFFER=*/0,
            .format = VIRGL_FORMAT_R8_UNORM,
            .bind = VIRGL_BIND_CUSTOM,
            .width = size,
            .height = 1,
            .depth = 1,
            .array_size = 0,
            .last_level = 0,
            .nr_samples = 0,
            .flags = 0,
        },
    };
    auto taskCompletedWaitable = EnqueueVirtioGpuTask(std::move(task));
    return TestingVirtGpuResource::createPipe(resourceId, shared_from_this(), taskCompletedWaitable, std::move(resourceBytes));
}

VirtGpuBlobPtr TestingVirtGpuDevice::createTexture(
        uint32_t width,
        uint32_t height,
        uint32_t drmFormat) {
    const uint32_t resourceId = mNextVirtioGpuResourceId++;

    // TODO: calculate for real.
    const uint32_t resourceSize = width * height * 4;

    auto resourceBytes = std::make_unique<uint8_t[]>(resourceSize);

    auto virglFormat = DrmFormatToVirglFormat(drmFormat);
    if (!virglFormat) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Unhandled format:" << drmFormat;
    }

    VirtioGpuTaskCreateResource task{
        .resourceId = resourceId,
        .resourceBytes = resourceBytes.get(),
        .params = {
            .handle = resourceId,
            .target = /*PIPE_TEXTURE_2D=*/2,
            .format = *virglFormat,
            .bind = VIRGL_BIND_CUSTOM,
            .width = width,
            .height = height,
            .depth = 1,
            .array_size = 1,
            .last_level = 0,
            .nr_samples = 0,
            .flags = 0,
        },
    };

    auto taskCompletedWaitable = EnqueueVirtioGpuTask(std::move(task));
    return TestingVirtGpuResource::createPipe(resourceId, shared_from_this(), taskCompletedWaitable, std::move(resourceBytes));
}

int TestingVirtGpuDevice::execBuffer(struct VirtGpuExecBuffer& execbuffer, VirtGpuBlobPtr blob) {
    std::optional<uint32_t> fence;

    if (execbuffer.flags & kFenceOut) {
        fence = CreateEmulatedFence();
    }

    VirtioGpuTaskExecBuffer task = {};

    task.commandBuffer.resize(execbuffer.command_size);
    std::memcpy(task.commandBuffer.data(), execbuffer.command, execbuffer.command_size);

    auto taskCompletedWaitable = EnqueueVirtioGpuTask(std::move(task), fence);

    if (blob) {
        if (auto* b = dynamic_cast<TestingVirtGpuResource*>(blob.get()); b != nullptr) {
            b->addPendingCommandWaitable(std::move(taskCompletedWaitable));
        } else {
            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                << "Execbuffer called with non-blob resource.";
        }
    }

    if (execbuffer.flags & kFenceOut) {
        execbuffer.handle.osHandle = *fence;
        execbuffer.handle.type = kFenceHandleSyncFd;
    }

    return 0;
}

VirtGpuBlobPtr TestingVirtGpuDevice::importBlob(const struct VirtGpuExternalHandle& handle) {
    GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Unimplemented";
    return nullptr;
}

std::shared_future<void> TestingVirtGpuDevice::transferFromHost(uint32_t resourceId,
                                                                uint32_t transferOffset,
                                                                uint32_t transferSize) {
    VirtioGpuTaskTransferFromHost task = {
        .resourceId = resourceId,
        .transferOffset = transferOffset,
        .transferSize = transferSize,
    };
    return EnqueueVirtioGpuTask(std::move(task));
}

std::shared_future<void> TestingVirtGpuDevice::transferToHost(uint32_t resourceId,
                                                              uint32_t transferOffset,
                                                              uint32_t transferSize) {
    VirtioGpuTaskTransferToHost task = {
        .resourceId = resourceId,
        .transferOffset = transferOffset,
        .transferSize = transferSize,
    };
    return EnqueueVirtioGpuTask(std::move(task));
}

int TestingVirtGpuDevice::WaitOnEmulatedFence(int fenceAsFileDescriptor,
                                              int timeoutMilliseconds) {
    uint32_t fenceId = static_cast<uint32_t>(fenceAsFileDescriptor);
    ALOGV("Waiting on fence:%d", (int)fenceId);

    std::shared_future<void> waitable;

    {
        std::lock_guard<std::mutex> lock(mVirtioGpuFencesMutex);

        auto fenceIt = mVirtioGpuFences.find(fenceId);
        if (fenceIt == mVirtioGpuFences.end()) {
            ALOGE("Fence:%d already signaled", (int)fenceId);
            return 0;
        }
        auto& fence = fenceIt->second;

        waitable = fence.waitable;
    }

    auto status = waitable.wait_for(std::chrono::milliseconds(timeoutMilliseconds));
    if (status == std::future_status::ready) {
        ALOGV("Finished waiting for fence:%d", (int)fenceId);
        return 0;
    } else {
        ALOGE("Timed out waiting for fence:%d", (int)fenceId);
        return -1;
    }
}

void TestingVirtGpuDevice::SignalEmulatedFence(uint32_t fenceId) {
    ALOGV("Signaling fence:%d", (int)fenceId);

    std::lock_guard<std::mutex> lock(mVirtioGpuFencesMutex);

    auto fenceIt = mVirtioGpuFences.find(fenceId);
    if (fenceIt == mVirtioGpuFences.end()) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Failed to find fence:" << fenceId;
    }
    auto& fenceInfo = fenceIt->second;
    fenceInfo.signaler.set_value();
}

void WriteFence(void* cookie, struct stream_renderer_fence* fence) {
    auto* device = reinterpret_cast<TestingVirtGpuDevice*>(cookie);
    device->SignalEmulatedFence(fence->fence_id);
}

uint32_t TestingVirtGpuDevice::CreateEmulatedFence() {
    const uint32_t fenceId = mNextVirtioGpuFenceId++;

    ALOGV("Creating fence:%d", (int)fenceId);

    std::lock_guard<std::mutex> lock(mVirtioGpuFencesMutex);

    auto [fenceIt, fenceCreated] = mVirtioGpuFences.emplace(fenceId, EmulatedFence{});
    if (!fenceCreated) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Attempting to recreate fence:" << fenceId;
    }

    auto& fenceInfo = fenceIt->second;
    fenceInfo.waitable = fenceInfo.signaler.get_future();

    return fenceId;
}

std::shared_future<void> TestingVirtGpuDevice::EnqueueVirtioGpuTask(
        VirtioGpuTask task,
        std::optional<uint32_t> fence) {
    std::promise<void> taskCompletedSignaler;
    std::shared_future<void> taskCompletedWaitable(taskCompletedSignaler.get_future());

    std::lock_guard<std::mutex> lock(mVirtioGpuTaskMutex);
    mVirtioGpuTasks.push(
        VirtioGpuTaskWithWaitable{
            .task = std::move(task),
            .taskCompletedSignaler = std::move(taskCompletedSignaler),
            .fence = fence,
        });

    return taskCompletedWaitable;
}

void TestingVirtGpuDevice::DoTask(VirtioGpuTaskCreateBlob task) {
    ALOGV("Performing task to create blob resource-id:%d", task.resourceId);

    int ret = stream_renderer_create_blob(kVirtioGpuContextId, task.resourceId, &task.params, nullptr, 0, nullptr);
    if (ret) {
        ALOGE("Failed to create blob.");
    }

    ALOGV("Performing task to create blob resource-id:%d - done", task.resourceId);
}

void TestingVirtGpuDevice::DoTask(VirtioGpuTaskCreateResource task) {
    ALOGV("Performing task to create resource resource:%d", task.resourceId);

    int ret = stream_renderer_resource_create(&task.params, nullptr, 0);
    if (ret) {
        ALOGE("Failed to create resource:%d", task.resourceId);
    }

    struct iovec iov = {
        .iov_base = task.resourceBytes,
        .iov_len = task.params.width,
    };
    ret = stream_renderer_resource_attach_iov(task.resourceId, &iov, 1);
    if (ret) {
        ALOGE("Failed to attach iov to resource:%d", task.resourceId);
    }

    ALOGV("Performing task to create resource resource:%d - done", task.resourceId);

    stream_renderer_ctx_attach_resource(kVirtioGpuContextId, task.resourceId);
}

void TestingVirtGpuDevice::DoTask(VirtioGpuTaskMap task) {
    ALOGV("Performing task to map resource resource:%d", task.resourceId);

    void* mapped = nullptr;

    int ret = stream_renderer_resource_map(task.resourceId, &mapped, nullptr);
    if (ret) {
        ALOGE("Failed to map resource:%d", task.resourceId);
        return;
    }

    task.resourceMappedPromise.set_value(reinterpret_cast<uint8_t*>(mapped));
    ALOGV("Performing task to map resource resource:%d - done", task.resourceId);
}

void TestingVirtGpuDevice::DoTask(VirtioGpuTaskExecBuffer task) {
    ALOGV("Performing task to execbuffer");

    if (task.commandBuffer.size() % 4 != 0) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Unaligned command?";
    }

    stream_renderer_command cmd = {
        .ctx_id = kVirtioGpuContextId,
        .cmd_size = static_cast<uint32_t>(task.commandBuffer.size()),
        .cmd = reinterpret_cast<uint8_t*>(task.commandBuffer.data()),
        .num_in_fences = 0,
        .fences = nullptr,
    };

    int ret = stream_renderer_submit_cmd(&cmd);
    if (ret) {
        ALOGE("Failed to execbuffer.");
    }

    ALOGV("Performing task to execbuffer - done");
}

void TestingVirtGpuDevice::DoTask(VirtioGpuTaskTransferFromHost task) {
    struct stream_renderer_box transferBox = {
        .x = task.transferOffset,
        .y = 0,
        .z = 0,
        .w = task.transferSize,
        .h = 1,
        .d = 1,
    };

    int ret = stream_renderer_transfer_read_iov(task.resourceId,
                                                    kVirtioGpuContextId,
                                                    /*level=*/0,
                                                    /*stride=*/0,
                                                    /*layer_stride=*/0,
                                                    &transferBox,
                                                    /*offset=*/0,
                                                    /*iov=*/nullptr,
                                                    /*iovec_cnt=*/0);
    if (ret) {
        ALOGE("Failed to transferFromHost() for resource:%" PRIu32, task.resourceId);
    }
}

void TestingVirtGpuDevice::DoTask(VirtioGpuTaskTransferToHost task) {
    struct stream_renderer_box transferBox = {
        .x = task.transferOffset,
        .y = 0,
        .z = 0,
        .w = task.transferSize,
        .h = 1,
        .d = 1,
    };

    int ret = stream_renderer_transfer_write_iov(task.resourceId,
                                                    kVirtioGpuContextId,
                                                    /*level=*/0,
                                                    /*stride=*/0,
                                                    /*layer_stride=*/0,
                                                    &transferBox,
                                                    /*offset=*/0,
                                                    /*iov=*/nullptr,
                                                    /*iovec_cnt=*/0);
    if (ret) {
        ALOGE("Failed to transferToHost() for resource:%" PRIu32, task.resourceId);
    }
}

void TestingVirtGpuDevice::DoTask(VirtioGpuTaskWithWaitable task) {
    std::visit(
        [this](auto&& work){
            using T = std::decay_t<decltype(work)>;
            if constexpr (std::is_same_v<T, VirtioGpuTaskCreateBlob>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskCreateResource>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskMap>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskExecBuffer>) {
                DoTask(std::move(work));
            }  else if constexpr (std::is_same_v<T, VirtioGpuTaskTransferFromHost>) {
                DoTask(std::move(work));
            }  else if constexpr (std::is_same_v<T, VirtioGpuTaskTransferToHost>) {
                DoTask(std::move(work));
            }
        }, task.task);

    if (task.fence) {
        const stream_renderer_fence fenceInfo = {
            .flags = STREAM_RENDERER_FLAG_FENCE_RING_IDX,
            .fence_id = *task.fence,
            .ctx_id = kVirtioGpuContextId,
            .ring_idx = 0,
        };
        int ret = stream_renderer_create_fence(&fenceInfo);
        if (ret) {
            ALOGE("Failed to create fence.");
        }
    }

    task.taskCompletedSignaler.set_value();
}

void TestingVirtGpuDevice::RunVirtioGpuTaskProcessingLoop() {
    while (!mShuttingDown.load()) {
        std::optional<VirtioGpuTaskWithWaitable> task;

        {
            std::lock_guard<std::mutex> lock(mVirtioGpuTaskMutex);
            if (!mVirtioGpuTasks.empty()) {
                task = std::move(mVirtioGpuTasks.front());
                mVirtioGpuTasks.pop();
            }
        }

        if (task) {
            DoTask(std::move(*task));
        }
    }
}

TestingAHardwareBuffer::TestingAHardwareBuffer(
        uint32_t width,
        uint32_t height,
        std::shared_ptr<TestingVirtGpuResource> resource)
    : mWidth(width),
      mHeight(height),
      mResource(resource) {}

uint32_t TestingAHardwareBuffer::getResourceId() const {
    return mResource->getResourceHandle();
}

uint32_t TestingAHardwareBuffer::getWidth() const {
    return mWidth;
}

uint32_t TestingAHardwareBuffer::getHeight() const {
    return mHeight;
}

int TestingAHardwareBuffer::getAndroidFormat() const {
    return /*AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM=*/1;
}

uint32_t TestingAHardwareBuffer::getDrmFormat() const {
    return DRM_FORMAT_ABGR8888;
}

AHardwareBuffer* TestingAHardwareBuffer::asAHardwareBuffer() {
    return reinterpret_cast<AHardwareBuffer*>(this);
}

EGLClientBuffer TestingAHardwareBuffer::asEglClientBuffer() {
    return reinterpret_cast<EGLClientBuffer>(this);
}

TestingVirtGpuGralloc::TestingVirtGpuGralloc(std::shared_ptr<TestingVirtGpuDevice> device)
    : mDevice(device) {}

uint32_t TestingVirtGpuGralloc::createColorBuffer(
        renderControl_client_context_t*,
        int width,
        int height,
        uint32_t glFormat) {
    auto drmFormat = GlFormatToDrmFormat(glFormat);
    if (!drmFormat) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Unhandled format:" << glFormat;
    }

    auto ahb = allocate(width, height, *drmFormat);

    uint32_t hostHandle = ahb->getResourceId();
    mAllocatedColorBuffers.emplace(hostHandle, std::move(ahb));
    return hostHandle;
}

int TestingVirtGpuGralloc::allocate(
        uint32_t width,
        uint32_t height,
        uint32_t format,
        uint64_t usage,
        AHardwareBuffer** outputAhb) {
    (void)width;
    (void)height;
    (void)format;
    (void)usage;
    (void)outputAhb;

    // TODO: support export flow?
    GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Unimplemented.";

    return 0;
}

std::unique_ptr<TestingAHardwareBuffer> TestingVirtGpuGralloc::allocate(
        uint32_t width,
        uint32_t height,
        uint32_t format) {
    auto resource = mDevice->createTexture(width, height, format);
    if (!resource) {
        return nullptr;
    }

    resource->wait();

    auto resourceTyped = std::dynamic_pointer_cast<TestingVirtGpuResource>(resource);
    if (!resourceTyped) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
            << "Failed to dynamic cast virtio gpu resource.";
    }

    return std::make_unique<TestingAHardwareBuffer>(width, height, std::move(resourceTyped));
}

void TestingVirtGpuGralloc::acquire(AHardwareBuffer* ahb) {
    // TODO
}

void TestingVirtGpuGralloc::release(AHardwareBuffer* ahb) {
    // TODO
}

uint32_t TestingVirtGpuGralloc::getHostHandle(const native_handle_t* handle) {
    const auto* ahb = reinterpret_cast<const TestingAHardwareBuffer*>(handle);
    return ahb->getResourceId();
}

uint32_t TestingVirtGpuGralloc::getHostHandle(const AHardwareBuffer* handle) {
    const auto* ahb = reinterpret_cast<const TestingAHardwareBuffer*>(handle);
    return ahb->getResourceId();
}

int TestingVirtGpuGralloc::getFormat(const native_handle_t* handle) {
    const auto* ahb = reinterpret_cast<const TestingAHardwareBuffer*>(handle);
    return ahb->getAndroidFormat();
}

int TestingVirtGpuGralloc::getFormat(const AHardwareBuffer* handle) {
    const auto* ahb = reinterpret_cast<const TestingAHardwareBuffer*>(handle);
    return ahb->getAndroidFormat();
}

uint32_t TestingVirtGpuGralloc::getFormatDrmFourcc(const AHardwareBuffer* handle) {
    const auto* ahb = reinterpret_cast<const TestingAHardwareBuffer*>(handle);
    return ahb->getDrmFormat();
}

size_t TestingVirtGpuGralloc::getAllocatedSize(const native_handle_t*) {
    GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Unimplemented.";
    return 0;
}

size_t TestingVirtGpuGralloc::getAllocatedSize(const AHardwareBuffer*) {
    GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Unimplemented.";
    return 0;
}

TestingANativeWindow::TestingANativeWindow(
        uint32_t width,
        uint32_t height,
        uint32_t format,
        std::vector<std::unique_ptr<TestingAHardwareBuffer>> buffers)
    : mWidth(width),
      mHeight(height),
      mFormat(format),
      mBuffers(std::move(buffers)) {
    for (auto& buffer : mBuffers) {
        mBufferQueue.push_back(QueuedAHB{
            .ahb = buffer.get(),
            .fence = -1,
        });
    }
}

EGLNativeWindowType TestingANativeWindow::asEglNativeWindowType() {
    return reinterpret_cast<EGLNativeWindowType>(this);
}

uint32_t TestingANativeWindow::getWidth() const {
    return mWidth;
}

uint32_t TestingANativeWindow::getHeight() const {
    return mHeight;
}

int TestingANativeWindow::getFormat() const {
    return mFormat;
}

int TestingANativeWindow::queueBuffer(EGLClientBuffer buffer, int fence) {
    auto ahb = reinterpret_cast<TestingAHardwareBuffer*>(buffer);

    mBufferQueue.push_back(QueuedAHB{
        .ahb = ahb,
        .fence = fence,
    });

    return 0;
}

int TestingANativeWindow::dequeueBuffer(EGLClientBuffer* buffer, int* fence) {
    auto queuedAhb = mBufferQueue.front();
    mBufferQueue.pop_front();

    *buffer = queuedAhb.ahb->asEglClientBuffer();
    *fence = queuedAhb.fence;
    return 0;
}

int TestingANativeWindow::cancelBuffer(EGLClientBuffer buffer) {
    auto ahb = reinterpret_cast<TestingAHardwareBuffer*>(buffer);

    mBufferQueue.push_back(QueuedAHB{
        .ahb = ahb,
        .fence = -1,
    });

    return 0;
}

bool TestingVirtGpuANativeWindowHelper::isValid(EGLNativeWindowType window) {
    // TODO: maybe a registry of valid TestingANativeWindow-s?
    return true;
}

bool TestingVirtGpuANativeWindowHelper::isValid(EGLClientBuffer buffer) {
    // TODO: maybe a registry of valid TestingAHardwareBuffer-s?
    return true;
}

void TestingVirtGpuANativeWindowHelper::acquire(EGLNativeWindowType window) {
    // TODO: maybe a registry of valid TestingANativeWindow-s?
}

void TestingVirtGpuANativeWindowHelper::release(EGLNativeWindowType window) {
    // TODO: maybe a registry of valid TestingANativeWindow-s?
}

void TestingVirtGpuANativeWindowHelper::acquire(EGLClientBuffer buffer) {
    // TODO: maybe a registry of valid TestingAHardwareBuffer-s?
}

void TestingVirtGpuANativeWindowHelper::release(EGLClientBuffer buffer) {
    // TODO: maybe a registry of valid TestingAHardwareBuffer-s?
}

int TestingVirtGpuANativeWindowHelper::getConsumerUsage(EGLNativeWindowType window, int* usage) {
    (void)window;
    (void)usage;
    return 0;
}
void TestingVirtGpuANativeWindowHelper::setUsage(EGLNativeWindowType window, int usage) {
    (void)window;
    (void)usage;
}

int TestingVirtGpuANativeWindowHelper::getWidth(EGLNativeWindowType window) {
    auto anw = reinterpret_cast<TestingANativeWindow*>(window);
    return anw->getWidth();
}

int TestingVirtGpuANativeWindowHelper::getHeight(EGLNativeWindowType window) {
    auto anw = reinterpret_cast<TestingANativeWindow*>(window);
    return anw->getHeight();
}

int TestingVirtGpuANativeWindowHelper::getWidth(EGLClientBuffer buffer) {
    auto ahb = reinterpret_cast<TestingAHardwareBuffer*>(buffer);
    return ahb->getWidth();
}

int TestingVirtGpuANativeWindowHelper::getHeight(EGLClientBuffer buffer) {
    auto ahb = reinterpret_cast<TestingAHardwareBuffer*>(buffer);
    return ahb->getHeight();
}

int TestingVirtGpuANativeWindowHelper::getFormat(EGLClientBuffer buffer, Gralloc* helper) {
    auto ahb = reinterpret_cast<TestingAHardwareBuffer*>(buffer);
    return ahb->getAndroidFormat();
}

void TestingVirtGpuANativeWindowHelper::setSwapInterval(EGLNativeWindowType window, int interval) {
    GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Unimplemented.";
}

int TestingVirtGpuANativeWindowHelper::queueBuffer(EGLNativeWindowType window, EGLClientBuffer buffer, int fence) {
    auto anw = reinterpret_cast<TestingANativeWindow*>(window);
    return anw->queueBuffer(buffer, fence);
}

int TestingVirtGpuANativeWindowHelper::dequeueBuffer(EGLNativeWindowType window, EGLClientBuffer* buffer, int* fence) {
    auto anw = reinterpret_cast<TestingANativeWindow*>(window);
    return anw->dequeueBuffer(buffer, fence);
}

int TestingVirtGpuANativeWindowHelper::cancelBuffer(EGLNativeWindowType window, EGLClientBuffer buffer) {
    auto anw = reinterpret_cast<TestingANativeWindow*>(window);
    return anw->cancelBuffer(buffer);
}

int TestingVirtGpuANativeWindowHelper::getHostHandle(EGLClientBuffer buffer, Gralloc*) {
    auto ahb = reinterpret_cast<TestingAHardwareBuffer*>(buffer);
    return ahb->getResourceId();
}

TestingVirtGpuSyncHelper::TestingVirtGpuSyncHelper(std::shared_ptr<TestingVirtGpuDevice> device)
    : mDevice(device) {}

int TestingVirtGpuSyncHelper::wait(int syncFd, int timeoutMilliseconds) {
    return mDevice->WaitOnEmulatedFence(syncFd, timeoutMilliseconds);
}

int TestingVirtGpuSyncHelper::dup(int syncFd) {
    // TODO update reference count
    return syncFd;
}

int TestingVirtGpuSyncHelper::close(int) {
    return 0;
}

std::string TestParams::ToString() const {
    std::string ret;
    ret += (with_gl ? "With" : "Without");
    ret += "Gl";
    ret += (with_vk ? "With" : "Without");
    ret += "Vk";
    return ret;
}

std::ostream& operator<<(std::ostream& os, const TestParams& params) {
    return os << params.ToString();
}

std::string GetTestName(const ::testing::TestParamInfo<TestParams>& info) {
    return info.param.ToString();
}

std::unique_ptr<GfxstreamEnd2EndTest::GuestGlDispatchTable> GfxstreamEnd2EndTest::SetupGuestGl() {
    const std::filesystem::path testDirectory = android::base::getProgramDirectory();
    const std::string eglLibPath = (testDirectory / "libEGL_emulation.so").string();
    const std::string gles2LibPath = (testDirectory / "libGLESv2_emulation.so").string();
    const std::string vkLibPath = (testDirectory / "vulkan.ranchu.so").string();

    void* eglLib = dlopen(eglLibPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!eglLib) {
        ALOGE("Failed to load Gfxstream EGL library from %s.", eglLibPath.c_str());
        return nullptr;
    }

    void* gles2Lib = dlopen(gles2LibPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!gles2Lib) {
        ALOGE("Failed to load Gfxstream GLES2 library from %s.", gles2LibPath.c_str());
        return nullptr;
    }

    using GenericFnType = void*(void);
    using GetProcAddrType = GenericFnType*(const char*);

    auto eglGetAddr = reinterpret_cast<GetProcAddrType*>(dlsym(eglLib, "eglGetProcAddress"));
    if (!eglGetAddr) {
        ALOGE("Failed to load Gfxstream EGL library from %s.", eglLibPath.c_str());
        return nullptr;
    }

    auto gl = std::make_unique<GuestGlDispatchTable>();

    #define LOAD_EGL_FUNCTION(return_type, function_name, signature) \
        gl-> function_name = reinterpret_cast< return_type (*) signature >(eglGetAddr( #function_name ));

    LIST_RENDER_EGL_FUNCTIONS(LOAD_EGL_FUNCTION)
    LIST_RENDER_EGL_EXTENSIONS_FUNCTIONS(LOAD_EGL_FUNCTION)

    #define LOAD_GLES2_FUNCTION(return_type, function_name, signature, callargs)    \
        gl-> function_name = reinterpret_cast< return_type (*) signature >(eglGetAddr( #function_name )); \
        if (!gl-> function_name) { \
            gl-> function_name = reinterpret_cast< return_type (*) signature >(dlsym(gles2Lib, #function_name)); \
        }

    LIST_GLES_FUNCTIONS(LOAD_GLES2_FUNCTION, LOAD_GLES2_FUNCTION)

    return gl;
}

std::unique_ptr<vkhpp::DynamicLoader> GfxstreamEnd2EndTest::SetupGuestVk() {
    const std::filesystem::path testDirectory = android::base::getProgramDirectory();
    const std::string vkLibPath = (testDirectory / "vulkan.ranchu.so").string();

    auto dl = std::make_unique<vkhpp::DynamicLoader>(vkLibPath);
    if (!dl->success()) {
        ALOGE("Failed to load Vulkan from: %s", vkLibPath.c_str());
        return nullptr;
    }

    auto getInstanceProcAddr = dl->getProcAddress<PFN_vkGetInstanceProcAddr>("vk_icdGetInstanceProcAddr");
    if (!getInstanceProcAddr) {
        ALOGE("Failed to load Vulkan vkGetInstanceProcAddr. %s", dlerror());
        return nullptr;
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstanceProcAddr);

    return dl;
}

void GfxstreamEnd2EndTest::SetUp() {
    const TestParams params = GetParam();

    mDevice = std::make_shared<TestingVirtGpuDevice>();
    VirtGpuDevice::setInstanceForTesting(mDevice.get());

    std::vector<stream_renderer_param> renderer_params{
        stream_renderer_param{
            .key = STREAM_RENDERER_PARAM_USER_DATA,
            .value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mDevice.get())),
        },
        stream_renderer_param{
            .key = STREAM_RENDERER_PARAM_FENCE_CALLBACK,
            .value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&WriteFence)),
        },
        stream_renderer_param{
            .key = STREAM_RENDERER_PARAM_RENDERER_FLAGS,
            .value =
                static_cast<uint64_t>(STREAM_RENDERER_FLAGS_USE_SURFACELESS_BIT) |
                (params.with_gl ? static_cast<uint64_t>(STREAM_RENDERER_FLAGS_USE_EGL_BIT  |
                                                        STREAM_RENDERER_FLAGS_USE_GLES_BIT) : 0 ) |
                (params.with_vk ? static_cast<uint64_t>(STREAM_RENDERER_FLAGS_USE_VK_BIT) : 0 ),
        },
        stream_renderer_param{
            .key = STREAM_RENDERER_PARAM_WIN0_WIDTH,
            .value = 32,
        },
        stream_renderer_param{
            .key = STREAM_RENDERER_PARAM_WIN0_HEIGHT,
            .value = 32,
        },
    };
    ASSERT_THAT(stream_renderer_init(renderer_params.data(), renderer_params.size()), Eq(0));

    const std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
    stream_renderer_context_create(kVirtioGpuContextId, name.size(), name.data(), 0);

    disableProcessPipeForTesting();

    // TODO:
    HostConnection::getOrCreate(kCapsetGfxStreamVulkan);

    mAnwHelper = std::make_unique<TestingVirtGpuANativeWindowHelper>();
    HostConnection::get()->setANativeWindowHelperForTesting(mAnwHelper.get());

    mGralloc = std::make_unique<TestingVirtGpuGralloc>(mDevice);
    HostConnection::get()->setGrallocHelperForTesting(mGralloc.get());

    mSync = std::make_unique<TestingVirtGpuSyncHelper>(mDevice);
    HostConnection::get()->setSyncHelperForTesting(mSync.get());

    if (params.with_gl) {
        mGl = SetupGuestGl();
        ASSERT_THAT(mGl, NotNull());
    }
    if (params.with_vk) {
        mVk = SetupGuestVk();
        ASSERT_THAT(mVk, NotNull());
    }
}

void GfxstreamEnd2EndTest::TearDownGuest() {
    mGralloc.reset();

    if (mGl) {
        EGLDisplay display = mGl->eglGetCurrentDisplay();
        if (display != EGL_NO_DISPLAY) {
            mGl->eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            mGl->eglTerminate(display);
        }
        mGl->eglReleaseThread();
        mGl.reset();
    }
    mVk.reset();

    HostConnection::exit();
    processPipeRestart();

    mAnwHelper.reset();
    mDevice.reset();
    mSync.reset();

    // Figure out more reliable way for guest shutdown to complete...
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

void GfxstreamEnd2EndTest::TearDownHost() {
    stream_renderer_context_destroy(kVirtioGpuContextId);
    stream_renderer_teardown();
}

void GfxstreamEnd2EndTest::TearDown() {
    TearDownGuest();
    TearDownHost();
}

std::unique_ptr<TestingANativeWindow> GfxstreamEnd2EndTest::CreateEmulatedANW(
        uint32_t width,
        uint32_t height) {
    std::vector<std::unique_ptr<TestingAHardwareBuffer>> buffers;
    for (int i = 0; i < 3; i++) {
        buffers.push_back(mGralloc->allocate(width, height, DRM_FORMAT_ABGR8888));
    }
    return std::make_unique<TestingANativeWindow>(width, height, DRM_FORMAT_ABGR8888, std::move(buffers));
}

void GfxstreamEnd2EndTest::SetUpEglContextAndSurface(
        uint32_t contextVersion,
        uint32_t width,
        uint32_t height,
        EGLDisplay* outDisplay,
        EGLContext* outContext,
        EGLSurface* outSurface) {
    ASSERT_THAT(contextVersion, AnyOf(Eq(2), Eq(3)))
        << "Invalid context version requested.";

    EGLDisplay display = mGl->eglGetDisplay(EGL_DEFAULT_DISPLAY);
    ASSERT_THAT(display, Not(Eq(EGL_NO_DISPLAY)));

    int versionMajor = 0;
    int versionMinor = 0;
    ASSERT_THAT(mGl->eglInitialize(display, &versionMajor, &versionMinor), IsTrue());

    ASSERT_THAT(mGl->eglBindAPI(EGL_OPENGL_ES_API), IsTrue());

    // clang-format off
    static const EGLint configAttributes[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    // clang-format on

    int numConfigs = 0;
    ASSERT_THAT(mGl->eglChooseConfig(display, configAttributes, nullptr, 1, &numConfigs), IsTrue());
    ASSERT_THAT(numConfigs, Gt(0));

    EGLConfig config = nullptr;
    ASSERT_THAT(mGl->eglChooseConfig(display, configAttributes, &config, 1, &numConfigs), IsTrue());
    ASSERT_THAT(config, Not(Eq(nullptr)));

    // clang-format off
    static const EGLint surfaceAttributes[] = {
        EGL_WIDTH,  static_cast<EGLint>(width),
        EGL_HEIGHT, static_cast<EGLint>(height),
        EGL_NONE,
    };
    // clang-format on

    EGLSurface surface = mGl->eglCreatePbufferSurface(display, config, surfaceAttributes);
    ASSERT_THAT(surface, Not(Eq(EGL_NO_SURFACE)));

    // clang-format off
    static const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, static_cast<EGLint>(contextVersion),
        EGL_NONE,
    };
    // clang-format on

    EGLContext context = mGl->eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    ASSERT_THAT(context, Not(Eq(EGL_NO_CONTEXT)));

    ASSERT_THAT(mGl->eglMakeCurrent(display, surface, surface, context), IsTrue());

    *outDisplay = display;
    *outContext = context;
    *outSurface = surface;
}

void GfxstreamEnd2EndTest::TearDownEglContextAndSurface(
        EGLDisplay display,
        EGLContext context,
        EGLSurface surface) {
    ASSERT_THAT(mGl->eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), IsTrue());
    ASSERT_THAT(mGl->eglDestroyContext(display, context), IsTrue());
    ASSERT_THAT(mGl->eglDestroySurface(display, surface), IsTrue());
}

GlExpected<GLuint> GfxstreamEnd2EndTest::SetUpShader(GLenum type, const std::string& source) {
    if (!mGl) {
        return android::base::unexpected("Gl not enabled for this test.");
    }

    GLuint shader = mGl->glCreateShader(type);
    if (!shader) {
        return android::base::unexpected("Failed to create shader.");
    }

    const GLchar* sourceTyped = (const GLchar*)source.c_str();
    const GLint sourceLength = source.size();
    mGl->glShaderSource(shader, 1, &sourceTyped, &sourceLength);
    mGl->glCompileShader(shader);

    GLenum err = mGl->glGetError();
    if (err != GL_NO_ERROR) {
        return android::base::unexpected("Failed to compile shader.");
    }

    GLint compileStatus;
    mGl->glGetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);

    if (compileStatus == GL_TRUE) {
        return shader;
    } else {
        GLint errorLogLength = 0;
        mGl->glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &errorLogLength);
        if (!errorLogLength) {
            errorLogLength = 512;
        }

        std::vector<GLchar> errorLog(errorLogLength);
        mGl->glGetShaderInfoLog(shader, errorLogLength, &errorLogLength, errorLog.data());

        const std::string errorString = errorLogLength == 0 ? "" : errorLog.data();
        ALOGE("Shader compilation failed with: \"%s\"", errorString.c_str());

        mGl->glDeleteShader(shader);
        return android::base::unexpected(errorString);
    }
}

GlExpected<GLuint> GfxstreamEnd2EndTest::SetUpProgram(
        const std::string& vertSource,
        const std::string& fragSource) {
    auto vertResult = SetUpShader(GL_VERTEX_SHADER, vertSource);
    if (!vertResult.ok()) {
        return vertResult;
    }
    auto vertShader = vertResult.value();

    auto fragResult = SetUpShader(GL_FRAGMENT_SHADER, fragSource);
    if (!fragResult.ok()) {
        return fragResult;
    }
    auto fragShader = fragResult.value();

    GLuint program = mGl->glCreateProgram();
    mGl->glAttachShader(program, vertShader);
    mGl->glAttachShader(program, fragShader);
    mGl->glLinkProgram(program);
    mGl->glDeleteShader(vertShader);
    mGl->glDeleteShader(fragShader);

    GLint linkStatus;
    mGl->glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    if (linkStatus == GL_TRUE) {
        return program;
    } else {
        GLint errorLogLength = 0;
        mGl->glGetProgramiv(program, GL_INFO_LOG_LENGTH, &errorLogLength);
        if (!errorLogLength) {
            errorLogLength = 512;
        }

        std::vector<char> errorLog(errorLogLength, 0);
        mGl->glGetProgramInfoLog(program, errorLogLength, nullptr, errorLog.data());

        const std::string errorString = errorLogLength == 0 ? "" : errorLog.data();
        ALOGE("Program link failed with: \"%s\"", errorString.c_str());

        mGl->glDeleteProgram(program);
        return android::base::unexpected(errorString);
    }
}

VkExpected<GfxstreamEnd2EndTest::TypicalVkTestEnvironment>
GfxstreamEnd2EndTest::SetUpTypicalVkTestEnvironment(uint32_t apiVersion) {
    const auto availableInstanceLayers = vkhpp::enumerateInstanceLayerProperties().value;
    ALOGV("Available instance layers:");
    for (const vkhpp::LayerProperties& layer : availableInstanceLayers) {
        ALOGV(" - %s", layer.layerName.data());
    }

    constexpr const bool kEnableValidationLayers = true;

    std::vector<const char*> requestedInstanceExtensions;
    std::vector<const char*> requestedInstanceLayers;
    if (kEnableValidationLayers) {
        requestedInstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    const vkhpp::ApplicationInfo applicationInfo{
        .pApplicationName = ::testing::UnitTest::GetInstance()->current_test_info()->name(),
        .applicationVersion = 1,
        .pEngineName = "Gfxstream Testing Engine",
        .engineVersion = 1,
        .apiVersion = apiVersion,
    };
    const vkhpp::InstanceCreateInfo instanceCreateInfo{
        .pApplicationInfo = &applicationInfo,
        .enabledLayerCount = static_cast<uint32_t>(requestedInstanceLayers.size()),
        .ppEnabledLayerNames = requestedInstanceLayers.data(),
        .enabledExtensionCount = static_cast<uint32_t>(requestedInstanceExtensions.size()),
        .ppEnabledExtensionNames = requestedInstanceExtensions.data(),
    };

    auto instance = VK_EXPECT_RV(vkhpp::createInstanceUnique(instanceCreateInfo));

    VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance);

    auto physicalDevices = VK_EXPECT_RV(instance->enumeratePhysicalDevices());
    ALOGV("Available physical devices:");
    for (const auto& physicalDevice : physicalDevices) {
        const auto physicalDeviceProps = physicalDevice.getProperties();
        ALOGV(" - %s", physicalDeviceProps.deviceName.data());
    }

    if (physicalDevices.empty()) {
        ALOGE("No physical devices available?");
        return android::base::unexpected(vkhpp::Result::eErrorUnknown);
    }

    auto physicalDevice = std::move(physicalDevices[0]);
    {
        const auto physicalDeviceProps = physicalDevice.getProperties();
        ALOGV("Selected physical device: %s", physicalDeviceProps.deviceName.data());
    }
    {
        const auto exts = VK_EXPECT_RV(physicalDevice.enumerateDeviceExtensionProperties());
        ALOGV("Available physical device extensions:");
        for (const auto& ext : exts) {
            ALOGV(" - %s", ext.extensionName.data());
        }
    }

    uint32_t graphicsQueueFamilyIndex = -1;
    {
        const auto props = physicalDevice.getQueueFamilyProperties();
        for (uint32_t i = 0; i < props.size(); i++) {
            const auto& prop = props[i];
            if (prop.queueFlags & vkhpp::QueueFlagBits::eGraphics) {
                graphicsQueueFamilyIndex = i;
                break;
            }
        }
    }
    if (graphicsQueueFamilyIndex == -1) {
        ALOGE("Failed to find graphics queue.");
        return android::base::unexpected(vkhpp::Result::eErrorUnknown);
    }

    const float queuePriority = 1.0f;
    const vkhpp::DeviceQueueCreateInfo deviceQueueCreateInfo = {
        .queueFamilyIndex = graphicsQueueFamilyIndex,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };
    const std::vector<const char*> deviceExtensions = {
        VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME,
    };
    const vkhpp::DeviceCreateInfo deviceCreateInfo = {
        .pQueueCreateInfos = &deviceQueueCreateInfo,
        .queueCreateInfoCount = 1,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size()),
        .ppEnabledExtensionNames = deviceExtensions.data(),
    };
    auto device = VK_EXPECT_RV(physicalDevice.createDeviceUnique(deviceCreateInfo));

    auto queue = device->getQueue(graphicsQueueFamilyIndex, 0);

    return TypicalVkTestEnvironment{
        .instance = std::move(instance),
        .physicalDevice = std::move(physicalDevice),
        .device = std::move(device),
        .queue = std::move(queue),
        .queueFamilyIndex = graphicsQueueFamilyIndex,
    };
}

}  // namespace tests
}  // namespace gfxstream
