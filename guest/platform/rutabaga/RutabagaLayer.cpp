/*
 * Copyright 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "RutabagaLayer.h"

#include <inttypes.h>
#include <log/log.h>

#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>

#include "gfxstream/virtio-gpu-gfxstream-renderer.h"
#include "gfxstream/virtio-gpu-gfxstream-renderer-unstable.h"

namespace gfxstream {
namespace {

constexpr const uint32_t kInvalidContextId = 0;

std::vector<std::string> Split(const std::string& s, const std::string& delimiters) {
    if (delimiters.empty()) {
        return {};
    }

    std::vector<std::string> result;

    size_t base = 0;
    size_t found;
    while (true) {
        found = s.find_first_of(delimiters, base);
        result.push_back(s.substr(base, found - base));
        if (found == s.npos) break;
        base = found + 1;
    }

    return result;
}

std::string Join(const std::vector<std::string>& things, const std::string& separator) {
    if (things.empty()) {
        return "";
    }

    std::ostringstream result;
    result << *things.begin();
    for (auto it = std::next(things.begin()); it != things.end(); ++it) {
        result << separator << *it;
    }
    return result.str();
}

}  // namespace

class EmulatedVirtioGpu::EmulatedVirtioGpuImpl {
   public:
    EmulatedVirtioGpuImpl();
    ~EmulatedVirtioGpuImpl();

    bool Init(bool withGl, bool withVk, bool withVkSnapshots, EmulatedVirtioGpu* parent);

    VirtGpuCaps GetCaps(VirtGpuCapset capset);

    std::optional<uint32_t> CreateContext();
    void DestroyContext(uint32_t contextId);

    uint8_t* Map(uint32_t resourceId);
    void Unmap(uint32_t resourceId);

    int ExecBuffer(uint32_t contextId, struct VirtGpuExecBuffer& execbuffer,
                   std::optional<uint32_t> blobResourceId);

    int Wait(uint32_t resourceId);

    int TransferFromHost(uint32_t contextId, uint32_t resourceId, uint32_t transferOffset,
                         uint32_t transferSize);
    int TransferToHost(uint32_t contextId, uint32_t resourceId, uint32_t transferOffset,
                       uint32_t transferSize);

    std::optional<uint32_t> CreateBlob(uint32_t contextId, const struct VirtGpuCreateBlob& params);
    std::optional<uint32_t> CreateVirglBlob(uint32_t contextId, uint32_t width, uint32_t height,
                                            uint32_t virglFormat);

    void DestroyResource(uint32_t contextId, uint32_t resourceId);

    uint32_t CreateEmulatedFence();

    void SignalEmulatedFence(uint32_t fenceId);

    int WaitOnEmulatedFence(int fenceAsFileDescriptor, int timeoutMilliseconds);

   private:
    struct VirtioGpuTaskContextAttachResource {
        uint32_t contextId;
        uint32_t resourceId;
    };
    struct VirtioGpuTaskContextDetachResource {
        uint32_t contextId;
        uint32_t resourceId;
    };
    struct VirtioGpuTaskCreateContext {
        uint32_t contextId;
        uint32_t contextInit;
        std::string contextName;
    };
    struct VirtioGpuTaskCreateBlob {
        uint32_t contextId;
        uint32_t resourceId;
        struct stream_renderer_create_blob params;
    };
    struct VirtioGpuTaskCreateResource {
        uint32_t contextId;
        uint32_t resourceId;
        uint8_t* resourceBytes;
        struct stream_renderer_resource_create_args params;
    };
    struct VirtioGpuTaskDestroyContext {
        uint32_t contextId;
    };
    struct VirtioGpuTaskMap {
        uint32_t resourceId;
        std::promise<uint8_t*> resourceMappedPromise;
    };
    struct VirtioGpuTaskExecBuffer {
        uint32_t contextId;
        std::vector<std::byte> commandBuffer;
    };
    struct VirtioGpuTaskTransferToHost {
        uint32_t contextId;
        uint32_t resourceId;
        uint32_t transferOffset;
        uint32_t transferSize;
    };
    struct VirtioGpuTaskTransferFromHost {
        uint32_t contextId;
        uint32_t resourceId;
        uint32_t transferOffset;
        uint32_t transferSize;
    };
    struct VirtioGpuTaskUnrefResource {
        uint32_t resourceId;
    };
    using VirtioGpuTask =
        std::variant<VirtioGpuTaskContextAttachResource, VirtioGpuTaskContextDetachResource,
                     VirtioGpuTaskCreateBlob, VirtioGpuTaskCreateContext,
                     VirtioGpuTaskCreateResource, VirtioGpuTaskDestroyContext, VirtioGpuTaskMap,
                     VirtioGpuTaskExecBuffer, VirtioGpuTaskTransferFromHost,
                     VirtioGpuTaskTransferToHost, VirtioGpuTaskUnrefResource>;
    struct VirtioGpuTaskWithWaitable {
        uint32_t contextId;
        VirtioGpuTask task;
        std::promise<void> taskCompletedSignaler;
        std::optional<uint32_t> fence;
    };

    std::shared_future<void> EnqueueVirtioGpuTask(uint32_t contextId, VirtioGpuTask task,
                                                  std::optional<uint32_t> fence = std::nullopt);
    void DoTask(VirtioGpuTaskContextAttachResource task);
    void DoTask(VirtioGpuTaskContextDetachResource task);
    void DoTask(VirtioGpuTaskCreateContext task);
    void DoTask(VirtioGpuTaskCreateBlob task);
    void DoTask(VirtioGpuTaskCreateResource task);
    void DoTask(VirtioGpuTaskDestroyContext task);
    void DoTask(VirtioGpuTaskMap task);
    void DoTask(VirtioGpuTaskExecBuffer task);
    void DoTask(VirtioGpuTaskTransferFromHost task);
    void DoTask(VirtioGpuTaskTransferToHost task);
    void DoTask(VirtioGpuTaskWithWaitable task);
    void DoTask(VirtioGpuTaskUnrefResource task);

    void RunVirtioGpuTaskProcessingLoop();

    std::atomic<uint32_t> mNextContextId{1};
    std::atomic<uint32_t> mNextVirtioGpuResourceId{1};
    std::atomic<uint32_t> mNextVirtioGpuFenceId{1};

    std::atomic_bool mShuttingDown{false};

    std::mutex mTasksMutex;
    std::queue<VirtioGpuTaskWithWaitable> mTasks;

    enum class EmulatedResourceType {
        kBlob,
        kPipe,
    };
    struct EmulatedResource {
        EmulatedResourceType type;

        std::mutex pendingWaitablesMutex;
        std::vector<std::shared_future<void>> pendingWaitables;

        // For non-blob resources, the guest shadow memory.
        std::unique_ptr<uint8_t[]> guestBytes;

        // For mappable blob resources, the host memory once it is mapped.
        std::shared_future<uint8_t*> mappedHostBytes;
    };
    std::mutex mResourcesMutex;
    std::unordered_map<uint32_t, EmulatedResource> mResources;

    EmulatedResource* CreateResource(uint32_t resourceId, EmulatedResourceType resourceType) {
        std::lock_guard<std::mutex> lock(mResourcesMutex);

        auto [it, created] = mResources.emplace(
            std::piecewise_construct, std::forward_as_tuple(resourceId), std::forward_as_tuple());
        if (!created) {
            ALOGE("Created resource %" PRIu32 " twice?", resourceId);
        }

        EmulatedResource* resource = &it->second;
        resource->type = resourceType;
        return resource;
    }

    EmulatedResource* GetResource(uint32_t resourceId) {
        std::lock_guard<std::mutex> lock(mResourcesMutex);

        auto it = mResources.find(resourceId);
        if (it == mResources.end()) {
            return nullptr;
        }

        return &it->second;
    }

    void DeleteResource(uint32_t resourceId) {
        std::lock_guard<std::mutex> lock(mResourcesMutex);
        mResources.erase(resourceId);
    }

    struct EmulatedFence {
        std::promise<void> signaler;
        std::shared_future<void> waitable;
    };
    std::mutex mVirtioGpuFencesMutex;
    std::unordered_map<uint32_t, EmulatedFence> mVirtioGpuFences;

    std::thread mWorkerThread;
};

EmulatedVirtioGpu::EmulatedVirtioGpuImpl::EmulatedVirtioGpuImpl()
    : mWorkerThread([this]() { RunVirtioGpuTaskProcessingLoop(); }) {}

EmulatedVirtioGpu::EmulatedVirtioGpuImpl::~EmulatedVirtioGpuImpl() {
    mShuttingDown = true;
    mWorkerThread.join();

    stream_renderer_teardown();
}

namespace {

void WriteFenceTrampoline(void* cookie, struct stream_renderer_fence* fence) {
    auto* gpu = reinterpret_cast<EmulatedVirtioGpu*>(cookie);
    gpu->SignalEmulatedFence(fence->fence_id);
}

}  // namespace

bool EmulatedVirtioGpu::EmulatedVirtioGpuImpl::Init(bool withGl, bool withVk, bool withVkSnapshots,
                                                    EmulatedVirtioGpu* parent) {
    std::vector<stream_renderer_param> renderer_params{
        stream_renderer_param{
            .key = STREAM_RENDERER_PARAM_USER_DATA,
            .value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(parent)),
        },
        stream_renderer_param{
            .key = STREAM_RENDERER_PARAM_FENCE_CALLBACK,
            .value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&WriteFenceTrampoline)),
        },
        stream_renderer_param{
            .key = STREAM_RENDERER_PARAM_RENDERER_FLAGS,
            .value =
                static_cast<uint64_t>(STREAM_RENDERER_FLAGS_USE_SURFACELESS_BIT) |
                (withGl ? static_cast<uint64_t>(STREAM_RENDERER_FLAGS_USE_EGL_BIT |
                                                STREAM_RENDERER_FLAGS_USE_GLES_BIT)
                        : 0) |
                (withVk ? static_cast<uint64_t>(STREAM_RENDERER_FLAGS_USE_VK_BIT) : 0) |
                (withVkSnapshots ? static_cast<uint64_t>(STREAM_RENDERER_FLAGS_VULKAN_SNAPSHOTS)
                                 : 0),
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
    return stream_renderer_init(renderer_params.data(), renderer_params.size()) == 0;
}

VirtGpuCaps EmulatedVirtioGpu::EmulatedVirtioGpuImpl::GetCaps(VirtGpuCapset capset) {
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

    stream_renderer_fill_caps(static_cast<uint32_t>(capset), 0, &caps.vulkanCapset);

    return caps;
}

std::optional<uint32_t> EmulatedVirtioGpu::EmulatedVirtioGpuImpl::CreateContext() {
    const uint32_t contextId = mNextContextId++;

    VirtioGpuTaskCreateContext task = {
        .contextId = contextId,
        .contextInit = 0,
        .contextName = "EmulatedVirtioGpu Context " + std::to_string(contextId),
    };
    EnqueueVirtioGpuTask(contextId, std::move(task));
    return contextId;
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DestroyContext(uint32_t contextId) {
    VirtioGpuTaskDestroyContext task = {
        .contextId = contextId,
    };
    EnqueueVirtioGpuTask(contextId, std::move(task));
}

uint8_t* EmulatedVirtioGpu::EmulatedVirtioGpuImpl::Map(uint32_t resourceId) {
    EmulatedResource* resource = GetResource(resourceId);
    if (resource == nullptr) {
        ALOGE("Failed to Map() resource %" PRIu32 ": not found.", resourceId);
        return nullptr;
    }

    uint8_t* mapped = nullptr;
    if (resource->type == EmulatedResourceType::kBlob) {
        if (!resource->mappedHostBytes.valid()) {
            ALOGE("Failed to Map() resource %" PRIu32
                  ": attempting to map blob "
                  "without mappable flag?",
                  resourceId);
            return nullptr;
        }
        mapped = resource->mappedHostBytes.get();
    } else if (resource->type == EmulatedResourceType::kPipe) {
        mapped = resource->guestBytes.get();
    }
    return mapped;
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::Unmap(uint32_t resourceId) {
    stream_renderer_resource_unmap(resourceId);
}

int EmulatedVirtioGpu::EmulatedVirtioGpuImpl::Wait(uint32_t resourceId) {
    EmulatedResource* resource = GetResource(resourceId);
    if (resource == nullptr) {
        ALOGE("Failed to Wait() on resource %" PRIu32 ": not found.", resourceId);
        return -1;
    }

    std::vector<std::shared_future<void>> pendingWaitables;
    {
        std::lock_guard<std::mutex> lock(resource->pendingWaitablesMutex);
        pendingWaitables = resource->pendingWaitables;
        resource->pendingWaitables.clear();
    }
    for (auto& waitable : pendingWaitables) {
        waitable.wait();
    }

    return 0;
}

int EmulatedVirtioGpu::EmulatedVirtioGpuImpl::TransferFromHost(uint32_t contextId,
                                                               uint32_t resourceId,
                                                               uint32_t transferOffset,
                                                               uint32_t transferSize) {
    EmulatedResource* resource = GetResource(resourceId);
    if (resource == nullptr) {
        ALOGE("Failed to TransferFromHost() on resource %" PRIu32 ": not found.", resourceId);
        return -1;
    }

    VirtioGpuTaskTransferFromHost task = {
        .contextId = contextId,
        .resourceId = resourceId,
        .transferOffset = transferOffset,
        .transferSize = transferSize,
    };
    auto waitable = EnqueueVirtioGpuTask(contextId, std::move(task));

    {
        std::lock_guard<std::mutex> lock(resource->pendingWaitablesMutex);
        resource->pendingWaitables.push_back(std::move(waitable));
    }

    return 0;
}

int EmulatedVirtioGpu::EmulatedVirtioGpuImpl::TransferToHost(uint32_t contextId,
                                                             uint32_t resourceId,
                                                             uint32_t transferOffset,
                                                             uint32_t transferSize) {
    EmulatedResource* resource = GetResource(resourceId);
    if (resource == nullptr) {
        ALOGE("Failed to TransferFromHost() on resource %" PRIu32 ": not found.", resourceId);
        return -1;
    }

    VirtioGpuTaskTransferToHost task = {
        .contextId = contextId,
        .resourceId = resourceId,
        .transferOffset = transferOffset,
        .transferSize = transferSize,
    };
    auto waitable = EnqueueVirtioGpuTask(contextId, std::move(task));

    {
        std::lock_guard<std::mutex> lock(resource->pendingWaitablesMutex);
        resource->pendingWaitables.push_back(std::move(waitable));
    }

    return 0;
}

std::optional<uint32_t> EmulatedVirtioGpu::EmulatedVirtioGpuImpl::CreateBlob(
    uint32_t contextId, const struct VirtGpuCreateBlob& blobCreate) {

    const uint32_t resourceId = mNextVirtioGpuResourceId++;

    ALOGV("Enquing task to create blob resource-id:%d size:%" PRIu64, resourceId, blobCreate.size);

    EmulatedResource* resource = CreateResource(resourceId, EmulatedResourceType::kBlob);

    VirtioGpuTaskCreateBlob createTask{
        .contextId = contextId,
        .resourceId = resourceId,
        .params =
            {
                .blob_mem = static_cast<uint32_t>(blobCreate.blobMem),
                .blob_flags = static_cast<uint32_t>(blobCreate.flags),
                .blob_id = blobCreate.blobId,
                .size = blobCreate.size,
            },
    };
    auto createBlobCompletedWaitable = EnqueueVirtioGpuTask(contextId, std::move(createTask));
    resource->pendingWaitables.push_back(std::move(createBlobCompletedWaitable));

    if (blobCreate.flags & kBlobFlagMappable) {
        std::promise<uint8_t*> mappedBytesPromise;
        std::shared_future<uint8_t*> mappedBytesWaitable = mappedBytesPromise.get_future();

        VirtioGpuTaskMap mapTask{
            .resourceId = resourceId,
            .resourceMappedPromise = std::move(mappedBytesPromise),
        };
        EnqueueVirtioGpuTask(contextId, std::move(mapTask));
        resource->mappedHostBytes = std::move(mappedBytesWaitable);
    }

    VirtioGpuTaskContextAttachResource attachTask{
        .contextId = contextId,
        .resourceId = resourceId,
    };
    EnqueueVirtioGpuTask(contextId, std::move(attachTask));

    return resourceId;
}

std::optional<uint32_t> EmulatedVirtioGpu::EmulatedVirtioGpuImpl::CreateVirglBlob(
    uint32_t contextId, uint32_t width, uint32_t height, uint32_t virglFormat) {

    const uint32_t resourceId = mNextVirtioGpuResourceId++;

    EmulatedResource* resource = CreateResource(resourceId, EmulatedResourceType::kPipe);

    uint32_t target = 0;
    uint32_t bind = 0;
    uint32_t bpp = 0;

    switch (virglFormat) {
        case VIRGL_FORMAT_R8G8B8A8_UNORM:
        case VIRGL_FORMAT_B8G8R8A8_UNORM:
            target = PIPE_TEXTURE_2D;
            bind = VIRGL_BIND_RENDER_TARGET;
            bpp = 4;
            break;
        case VIRGL_FORMAT_B5G6R5_UNORM:
            target = PIPE_TEXTURE_2D;
            bind = VIRGL_BIND_RENDER_TARGET;
            bpp = 2;
            break;
        case VIRGL_FORMAT_R8G8B8_UNORM:
            target = PIPE_TEXTURE_2D;
            bind = VIRGL_BIND_RENDER_TARGET;
            bpp = 3;
            break;
        case VIRGL_FORMAT_R8_UNORM:
            target = PIPE_BUFFER;
            bind = VIRGL_BIND_CUSTOM;
            bpp = 1;
            break;
        default:
            ALOGE("Unknown virgl format %u", virglFormat);
            return {};
    }

    resource->guestBytes = std::make_unique<uint8_t[]>(width * height * bpp);

    VirtioGpuTaskCreateResource task{
        .contextId = contextId,
        .resourceId = resourceId,
        .resourceBytes = resource->guestBytes.get(),
        .params =
            {
                .handle = resourceId,
                .target = target,
                .format = virglFormat,
                .bind = bind,
                .width = width,
                .height = height,
                .depth = 1,
                .array_size = 1,
                .last_level = 0,
                .nr_samples = 0,
                .flags = 0,
            },
    };
    auto taskCompletedWaitable = EnqueueVirtioGpuTask(contextId, std::move(task));
    resource->pendingWaitables.push_back(std::move(taskCompletedWaitable));

    VirtioGpuTaskContextAttachResource attachTask{
        .contextId = contextId,
        .resourceId = resourceId,
    };
    EnqueueVirtioGpuTask(contextId, std::move(attachTask));

    return resourceId;
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DestroyResource(uint32_t contextId,
                                                               uint32_t resourceId) {
    DeleteResource(resourceId);

    VirtioGpuTaskUnrefResource unrefTask{
        .resourceId = resourceId,
    };
    EnqueueVirtioGpuTask(contextId, std::move(unrefTask));

    VirtioGpuTaskContextDetachResource detachTask{
        .contextId = contextId,
        .resourceId = resourceId,
    };
    EnqueueVirtioGpuTask(contextId, std::move(detachTask));
}

int EmulatedVirtioGpu::EmulatedVirtioGpuImpl::ExecBuffer(uint32_t contextId,
                                                         struct VirtGpuExecBuffer& execbuffer,
                                                         std::optional<uint32_t> blobResourceId) {
    std::optional<uint32_t> fence;

    if (execbuffer.flags & kFenceOut) {
        fence = CreateEmulatedFence();
    }

    const VirtioGpuTaskExecBuffer task = {
        .contextId = contextId,
        .commandBuffer = std::vector<std::byte>(
            reinterpret_cast<std::byte*>(execbuffer.command),
            reinterpret_cast<std::byte*>(execbuffer.command) + execbuffer.command_size),
    };
    auto taskCompletedWaitable = EnqueueVirtioGpuTask(contextId, std::move(task), fence);

    if (blobResourceId) {
        EmulatedResource* resource = GetResource(*blobResourceId);
        if (resource == nullptr) {
            ALOGE("Failed to ExecBuffer() with resource %" PRIu32 ": not found.", *blobResourceId);
            return -1;
        }

        {
            std::lock_guard<std::mutex> lock(resource->pendingWaitablesMutex);
            resource->pendingWaitables.push_back(std::move(taskCompletedWaitable));
        }
    }

    if (execbuffer.flags & kFenceOut) {
        execbuffer.handle.osHandle = *fence;
        execbuffer.handle.type = kFenceHandleSyncFd;
    }

    return 0;
}

int EmulatedVirtioGpu::EmulatedVirtioGpuImpl::WaitOnEmulatedFence(int fenceAsFileDescriptor,
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

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::SignalEmulatedFence(uint32_t fenceId) {
    ALOGV("Signaling fence:%d", (int)fenceId);

    std::lock_guard<std::mutex> lock(mVirtioGpuFencesMutex);

    auto fenceIt = mVirtioGpuFences.find(fenceId);
    if (fenceIt == mVirtioGpuFences.end()) {
        ALOGE("Failed to find fence %" PRIu32, fenceId);
        return;
    }
    auto& fenceInfo = fenceIt->second;
    fenceInfo.signaler.set_value();
}

uint32_t EmulatedVirtioGpu::EmulatedVirtioGpuImpl::CreateEmulatedFence() {
    const uint32_t fenceId = mNextVirtioGpuFenceId++;

    ALOGV("Creating fence:%d", (int)fenceId);

    std::lock_guard<std::mutex> lock(mVirtioGpuFencesMutex);

    auto [fenceIt, fenceCreated] = mVirtioGpuFences.emplace(fenceId, EmulatedFence{});
    if (!fenceCreated) {
        ALOGE("Attempting to recreate fence %" PRIu32, fenceId);
    }

    auto& fenceInfo = fenceIt->second;
    fenceInfo.waitable = fenceInfo.signaler.get_future();

    return fenceId;
}

std::shared_future<void> EmulatedVirtioGpu::EmulatedVirtioGpuImpl::EnqueueVirtioGpuTask(
    uint32_t contextId, VirtioGpuTask task, std::optional<uint32_t> fence) {
    std::promise<void> taskCompletedSignaler;
    std::shared_future<void> taskCompletedWaitable(taskCompletedSignaler.get_future());

    std::lock_guard<std::mutex> lock(mTasksMutex);
    mTasks.push(VirtioGpuTaskWithWaitable{
        .contextId = contextId,
        .task = std::move(task),
        .taskCompletedSignaler = std::move(taskCompletedSignaler),
        .fence = fence,
    });

    return taskCompletedWaitable;
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DoTask(VirtioGpuTaskContextAttachResource task) {
    ALOGV("Performing task to attach resource-id:%d to context-id:%d", task.resourceId,
          task.contextId);

    stream_renderer_ctx_attach_resource(task.contextId, task.resourceId);

    ALOGV("Performing task to attach resource-id:%d to context-id:%d - done", task.resourceId,
          task.contextId);
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DoTask(VirtioGpuTaskContextDetachResource task) {
    ALOGV("Performing task to detach resource-id:%d to context-id:%d", task.resourceId,
          task.contextId);

    stream_renderer_ctx_detach_resource(task.contextId, task.resourceId);

    ALOGV("Performing task to detach resource-id:%d to context-id:%d - done", task.resourceId,
          task.contextId);
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DoTask(VirtioGpuTaskCreateBlob task) {
    ALOGV("Performing task to create blob resource-id:%d", task.resourceId);

    int ret = stream_renderer_create_blob(task.contextId, task.resourceId, &task.params,
                                          /*iovecs=*/nullptr,
                                          /*num_iovs=*/0,
                                          /*handle=*/nullptr);
    if (ret) {
        ALOGE("Failed to create blob.");
    }

    ALOGV("Performing task to create blob resource-id:%d - done", task.resourceId);
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DoTask(VirtioGpuTaskCreateContext task) {
    ALOGV("Performing task to create context-id:%" PRIu32 " context-init:%" PRIu32
          " context-name:%s",
          task.contextId, task.contextInit, task.contextName.c_str());

    int ret = stream_renderer_context_create(task.contextId, task.contextName.size(),
                                             task.contextName.data(), task.contextInit);
    if (ret) {
        ALOGE("Failed to create context-id:%" PRIu32 ".", task.contextId);
        return;
    }

    ALOGV("Performing task to create context-id:%" PRIu32 " context-init:%" PRIu32
          " context-name:%s - done",
          task.contextId, task.contextInit, task.contextName.c_str());
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DoTask(VirtioGpuTaskCreateResource task) {
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

    stream_renderer_ctx_attach_resource(task.contextId, task.resourceId);
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DoTask(VirtioGpuTaskDestroyContext task) {
    ALOGV("Performing task to destroy context-id:%" PRIu32, task.contextId);

    stream_renderer_context_destroy(task.contextId);

    ALOGV("Performing task to destroy context-id:%" PRIu32 " - done", task.contextId);
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DoTask(VirtioGpuTaskMap task) {
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

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DoTask(VirtioGpuTaskExecBuffer task) {
    ALOGV("Performing task to execbuffer");

    if (task.commandBuffer.size() % 4 != 0) {
        ALOGE("Unaligned command buffer?");
        return;
    }

    stream_renderer_command cmd = {
        .ctx_id = task.contextId,
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

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DoTask(VirtioGpuTaskTransferFromHost task) {
    struct stream_renderer_box transferBox = {
        .x = task.transferOffset,
        .y = 0,
        .z = 0,
        .w = task.transferSize,
        .h = 1,
        .d = 1,
    };

    int ret = stream_renderer_transfer_read_iov(task.resourceId, task.contextId,
                                                /*level=*/0,
                                                /*stride=*/0,
                                                /*layer_stride=*/0, &transferBox,
                                                /*offset=*/0,
                                                /*iov=*/nullptr,
                                                /*iovec_cnt=*/0);
    if (ret) {
        ALOGE("Failed to transferFromHost() for resource:%" PRIu32, task.resourceId);
    }
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DoTask(VirtioGpuTaskTransferToHost task) {
    struct stream_renderer_box transferBox = {
        .x = task.transferOffset,
        .y = 0,
        .z = 0,
        .w = task.transferSize,
        .h = 1,
        .d = 1,
    };

    int ret = stream_renderer_transfer_write_iov(task.resourceId, task.contextId,
                                                 /*level=*/0,
                                                 /*stride=*/0,
                                                 /*layer_stride=*/0, &transferBox,
                                                 /*offset=*/0,
                                                 /*iov=*/nullptr,
                                                 /*iovec_cnt=*/0);
    if (ret) {
        ALOGE("Failed to transferToHost() for resource:%" PRIu32, task.resourceId);
    }
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DoTask(VirtioGpuTaskUnrefResource task) {
    stream_renderer_resource_unref(task.resourceId);
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::DoTask(VirtioGpuTaskWithWaitable task) {
    std::visit(
        [this](auto&& work) {
            using T = std::decay_t<decltype(work)>;
            if constexpr (std::is_same_v<T, VirtioGpuTaskContextAttachResource>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskContextDetachResource>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskCreateBlob>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskCreateContext>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskCreateResource>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskDestroyContext>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskMap>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskExecBuffer>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskTransferFromHost>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskTransferToHost>) {
                DoTask(std::move(work));
            } else if constexpr (std::is_same_v<T, VirtioGpuTaskUnrefResource>) {
                DoTask(std::move(work));
            }
        },
        task.task);

    if (task.fence) {
        const stream_renderer_fence fenceInfo = {
            .flags = STREAM_RENDERER_FLAG_FENCE_RING_IDX,
            .fence_id = *task.fence,
            .ctx_id = task.contextId,
            .ring_idx = 0,
        };
        int ret = stream_renderer_create_fence(&fenceInfo);
        if (ret) {
            ALOGE("Failed to create fence.");
        }
    }

    task.taskCompletedSignaler.set_value();
}

void EmulatedVirtioGpu::EmulatedVirtioGpuImpl::RunVirtioGpuTaskProcessingLoop() {
    while (!mShuttingDown.load()) {
        std::optional<VirtioGpuTaskWithWaitable> task;

        {
            std::lock_guard<std::mutex> lock(mTasksMutex);
            if (!mTasks.empty()) {
                task = std::move(mTasks.front());
                mTasks.pop();
            }
        }

        if (task) {
            DoTask(std::move(*task));
        }
    }
}

namespace {

EmulatedVirtioGpu* sInstance = nullptr;

}  // namespace

EmulatedVirtioGpu::EmulatedVirtioGpu()
    : mImpl{std::make_unique<EmulatedVirtioGpu::EmulatedVirtioGpuImpl>()} {}

/*static*/
EmulatedVirtioGpu& EmulatedVirtioGpu::Get() {
    if (sInstance == nullptr) {
        sInstance = new EmulatedVirtioGpu();

        bool withGl = false;
        bool withVk = true;
        bool withVkSnapshots = false;

        struct Option {
            std::string env;
            bool* val;
        };
        const std::vector<Option> options = {
            {"GFXSTREAM_EMULATED_VIRTIO_GPU_WITH_GL", &withGl},
            {"GFXSTREAM_EMULATED_VIRTIO_GPU_WITH_VK", &withVk},
            {"GFXSTREAM_EMULATED_VIRTIO_GPU_WITH_VK_SNAPSHOTS", &withVkSnapshots},
        };
        for (const Option option : options) {
            const char* val = std::getenv(option.env.c_str());
            if (val != nullptr && (val[0] == 'Y' || val[0] == 'y')) {
                *option.val = true;
            }
        }

        ALOGE("Initializing withGl:%d withVk:%d withVkSnapshots:%d", withGl, withVk,
              withVkSnapshots);
        if (!sInstance->Init(withGl, withVk, withVkSnapshots)) {
            ALOGE("Failed to initialize EmulatedVirtioGpu.");
        }
    }
    return *sInstance;
}

/*static*/
void EmulatedVirtioGpu::Reset() {
    if (sInstance != nullptr) {
        delete sInstance;
        sInstance = nullptr;
    }
}

bool EmulatedVirtioGpu::Init(bool withGl, bool withVk, bool withVkSnapshots) {
    return mImpl->Init(withGl, withVk, withVkSnapshots, this);
}

std::optional<uint32_t> EmulatedVirtioGpu::CreateContext() { return mImpl->CreateContext(); }

void EmulatedVirtioGpu::DestroyContext(uint32_t contextId) { mImpl->DestroyContext(contextId); }

VirtGpuCaps EmulatedVirtioGpu::GetCaps(VirtGpuCapset capset) { return mImpl->GetCaps(capset); }

uint8_t* EmulatedVirtioGpu::Map(uint32_t resourceId) { return mImpl->Map(resourceId); }

void EmulatedVirtioGpu::Unmap(uint32_t resourceId) { mImpl->Unmap(resourceId); }

int EmulatedVirtioGpu::ExecBuffer(uint32_t contextId, struct VirtGpuExecBuffer& execbuffer,
                                  std::optional<uint32_t> blobResourceId) {
    return mImpl->ExecBuffer(contextId, execbuffer, blobResourceId);
}

int EmulatedVirtioGpu::Wait(uint32_t resourceId) { return mImpl->Wait(resourceId); }

int EmulatedVirtioGpu::TransferFromHost(uint32_t contextId, uint32_t resourceId, uint32_t offset,
                                        uint32_t size) {
    return mImpl->TransferFromHost(contextId, resourceId, offset, size);
}

int EmulatedVirtioGpu::TransferToHost(uint32_t contextId, uint32_t resourceId, uint32_t offset,
                                      uint32_t size) {
    return mImpl->TransferToHost(contextId, resourceId, offset, size);
}

std::optional<uint32_t> EmulatedVirtioGpu::CreateBlob(uint32_t contextId,
                                                      const struct VirtGpuCreateBlob& params) {
    return mImpl->CreateBlob(contextId, params);
}

std::optional<uint32_t> EmulatedVirtioGpu::CreateVirglBlob(uint32_t contextId, uint32_t width,
                                                           uint32_t height, uint32_t virglFormat) {
    return mImpl->CreateVirglBlob(contextId, width, height, virglFormat);
}

void EmulatedVirtioGpu::DestroyResource(uint32_t contextId, uint32_t resourceId) {
    mImpl->DestroyResource(contextId, resourceId);
}

int EmulatedVirtioGpu::WaitOnEmulatedFence(int fenceAsFileDescriptor, int timeoutMilliseconds) {
    return mImpl->WaitOnEmulatedFence(fenceAsFileDescriptor, timeoutMilliseconds);
}

void EmulatedVirtioGpu::SignalEmulatedFence(int fenceId) { mImpl->SignalEmulatedFence(fenceId); }

void ResetEmulatedVirtioGpu() { EmulatedVirtioGpu::Reset(); }

}  // namespace gfxstream
