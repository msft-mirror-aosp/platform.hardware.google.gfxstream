// Copyright (C) 2024 The Android Open Source Project
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

#include "VirtioGpuFrontend.h"

#include <vulkan/vulkan.h>

#include "FrameBuffer.h"
#include "FrameworkFormats.h"
#include "VkCommonOperations.h"
#include "aemu/base/ManagedDescriptor.hpp"
#include "aemu/base/memory/SharedMemory.h"
#include "aemu/base/threads/WorkerThread.h"
#include "gfxstream/host/Tracing.h"
#include "host-common/AddressSpaceService.h"
#include "host-common/address_space_device.h"
#include "host-common/address_space_device_control_ops.h"
#include "host-common/opengles.h"
#include "virtgpu_gfxstream_protocol.h"

namespace gfxstream {
namespace host {
namespace {

using android::base::DescriptorType;
using android::base::SharedMemory;
#ifdef GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT
using gfxstream::host::snapshot::VirtioGpuContextSnapshot;
using gfxstream::host::snapshot::VirtioGpuFrontendSnapshot;
using gfxstream::host::snapshot::VirtioGpuResourceSnapshot;
#endif  // ifdef GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT

struct VirtioGpuCmd {
    uint32_t op;
    uint32_t cmdSize;
    unsigned char buf[0];
} __attribute__((packed));

static uint64_t convert32to64(uint32_t lo, uint32_t hi) {
    return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

}  // namespace

class CleanupThread {
   public:
    using GenericCleanup = std::function<void()>;

    CleanupThread()
        : mWorker([](CleanupTask task) {
              return std::visit(
                  [](auto&& work) {
                      using T = std::decay_t<decltype(work)>;
                      if constexpr (std::is_same_v<T, GenericCleanup>) {
                          work();
                          return android::base::WorkerProcessingResult::Continue;
                      } else if constexpr (std::is_same_v<T, Exit>) {
                          return android::base::WorkerProcessingResult::Stop;
                      }
                  },
                  std::move(task));
          }) {
        mWorker.start();
    }

    ~CleanupThread() { stop(); }

    // CleanupThread is neither copyable nor movable.
    CleanupThread(const CleanupThread& other) = delete;
    CleanupThread& operator=(const CleanupThread& other) = delete;
    CleanupThread(CleanupThread&& other) = delete;
    CleanupThread& operator=(CleanupThread&& other) = delete;

    void enqueueCleanup(GenericCleanup command) { mWorker.enqueue(std::move(command)); }

    void stop() {
        mWorker.enqueue(Exit{});
        mWorker.join();
    }

   private:
    struct Exit {};
    using CleanupTask = std::variant<GenericCleanup, Exit>;
    android::base::WorkerThread<CleanupTask> mWorker;
};

VirtioGpuFrontend::VirtioGpuFrontend() = default;

int VirtioGpuFrontend::init(void* cookie, gfxstream::host::FeatureSet features,
                            stream_renderer_fence_callback fence_callback) {
    stream_renderer_debug("cookie: %p", cookie);
    mCookie = cookie;
    mFeatures = features;
    mFenceCallback = fence_callback;
    mAddressSpaceDeviceControlOps = get_address_space_device_control_ops();
    if (!mAddressSpaceDeviceControlOps) {
        stream_renderer_error("Could not get address space device control ops!");
        return -EINVAL;
    }
    mVirtioGpuTimelines = VirtioGpuTimelines::create(true);
    mVirtioGpuTimelines = VirtioGpuTimelines::create(true);

#if !defined(_WIN32)
    mPageSize = getpagesize();
#endif

    mCleanupThread.reset(new CleanupThread());

    return 0;
}

void VirtioGpuFrontend::teardown() { mCleanupThread.reset(); }

int VirtioGpuFrontend::resetPipe(VirtioGpuContextId contextId, GoldfishHostPipe* hostPipe) {
    stream_renderer_debug("reset pipe for context %u to hostpipe %p", contextId, hostPipe);

    auto contextIt = mContexts.find(contextId);
    if (contextIt == mContexts.end()) {
        stream_renderer_error("failed to reset pipe: context %u not found.", contextId);
        return -EINVAL;
    }
    auto& context = contextIt->second;
    context.hostPipe = hostPipe;

    // Also update any resources associated with it
    auto contextResourcesIt = mContextResources.find(contextId);
    if (contextResourcesIt == mContextResources.end()) {
        return 0;
    }
    const auto& contextResources = contextResourcesIt->second;

    for (auto resourceId : contextResources) {
        auto resourceIt = mResources.find(resourceId);
        if (resourceIt == mResources.end()) {
            stream_renderer_error("failed to reset pipe: resource %d not found.", resourceId);
            return -EINVAL;
        }
        auto& resource = resourceIt->second;
        resource.SetHostPipe(hostPipe);
    }

    return 0;
}

int VirtioGpuFrontend::createContext(VirtioGpuCtxId ctx_id, uint32_t nlen, const char* name,
                                     uint32_t context_init) {
    std::string contextName(name, nlen);

    stream_renderer_debug("ctxid: %u len: %u name: %s", ctx_id, nlen, contextName.c_str());
    auto ops = ensureAndGetServiceOps();
    auto hostPipe =
        ops->guest_open_with_flags(reinterpret_cast<GoldfishHwPipe*>(ctx_id), 0x1 /* is virtio */);

    if (!hostPipe) {
        stream_renderer_error("failed to create hw pipe!");
        return -EINVAL;
    }
    std::unordered_map<uint32_t, uint32_t> map;
    std::unordered_map<uint32_t, struct stream_renderer_resource_create_args> blobMap;

    VirtioGpuContext res = {
        std::move(contextName),  // contextName
        context_init,            // capsetId
        ctx_id,                  // ctxId
        hostPipe,                // hostPipe
        map,                     // resourceId --> ASG handle map
        blobMap,                 // blobId -> resource create args
    };

    stream_renderer_debug("initial host pipe for ctxid %u: %p", ctx_id, hostPipe);
    mContexts[ctx_id] = res;
    android_onGuestGraphicsProcessCreate(ctx_id);
    return 0;
}

int VirtioGpuFrontend::destroyContext(VirtioGpuCtxId contextId) {
    stream_renderer_debug("ctxid: %u", contextId);

    auto contextIt = mContexts.find(contextId);
    if (contextIt == mContexts.end()) {
        stream_renderer_error("could not find context handle %u", contextId);
        return -EINVAL;
    }
    auto& context = contextIt->second;

    for (auto const& [resourceId, handle] : context.addressSpaceHandles) {
        // Note: this can hang as is but this has only been observed to
        // happen during shutdown. See b/329287602#comment8.
        mAddressSpaceDeviceControlOps->destroy_handle(handle);
    }

    auto hostPipe = context.hostPipe;
    if (!hostPipe) {
        stream_renderer_error("0 is not a valid hostpipe");
        return -EINVAL;
    }

    auto ops = ensureAndGetServiceOps();
    ops->guest_close(hostPipe, GOLDFISH_PIPE_CLOSE_GRACEFUL);

    android_cleanupProcGLObjects(contextId);
    mContexts.erase(contextIt);
    return 0;
}

int VirtioGpuFrontend::setContextAddressSpaceHandleLocked(VirtioGpuCtxId ctxId, uint32_t handle,
                                                          uint32_t resourceId) {
    auto ctxIt = mContexts.find(ctxId);
    if (ctxIt == mContexts.end()) {
        stream_renderer_error("ctx id %u is not found", ctxId);
        return -EINVAL;
    }

    auto& ctxEntry = ctxIt->second;
    ctxEntry.addressSpaceHandles[resourceId] = handle;
    return 0;
}

uint32_t VirtioGpuFrontend::getAddressSpaceHandleLocked(VirtioGpuCtxId ctxId, uint32_t resourceId) {
    auto ctxIt = mContexts.find(ctxId);
    if (ctxIt == mContexts.end()) {
        stream_renderer_error("ctx id %u is not found", ctxId);
        return -EINVAL;
    }

    auto& ctxEntry = ctxIt->second;

    if (!ctxEntry.addressSpaceHandles.count(resourceId)) {
        stream_renderer_error("ASG context with resource id %u", resourceId);
        return -EINVAL;
    }

    return ctxEntry.addressSpaceHandles[resourceId];
}

#define DECODE(variable, type, input) \
    type variable = {};               \
    memcpy(&variable, input, sizeof(type));

int VirtioGpuFrontend::addressSpaceProcessCmd(VirtioGpuCtxId ctxId, uint32_t* dwords) {
    DECODE(header, gfxstream::gfxstreamHeader, dwords)

    switch (header.opCode) {
        case GFXSTREAM_CONTEXT_CREATE: {
            DECODE(contextCreate, gfxstream::gfxstreamContextCreate, dwords)

            auto resourceIt = mResources.find(contextCreate.resourceId);
            if (resourceIt == mResources.end()) {
                stream_renderer_error("ASG coherent resource %u not found",
                                      contextCreate.resourceId);
                return -EINVAL;
            }
            auto& resource = resourceIt->second;

            void* resourceHva = nullptr;
            uint64_t resourceHvaSize = 0;
            if (resource.Map(&resourceHva, &resourceHvaSize) != 0) {
                stream_renderer_error("failed to create ASG: failed to map resource %u",
                                      contextCreate.resourceId);
                return -EINVAL;
            }

            auto contextIt = mContexts.find(ctxId);
            if (contextIt == mContexts.end()) {
                stream_renderer_error("ctx id %u not found", ctxId);
                return -EINVAL;
            }
            auto& context = contextIt->second;

            std::string name = context.name + "-" + std::to_string(contextCreate.resourceId);

            // Note: resource ids can not be used as ASG handles because ASGs may outlive the
            // containing resource due asynchronous ASG destruction.
            uint32_t handle = mAddressSpaceDeviceControlOps->gen_handle();

            struct AddressSpaceCreateInfo createInfo = {
                .handle = handle,
                .type = android::emulation::VirtioGpuGraphics,
                .createRenderThread = true,
                .externalAddr = resourceHva,
                .externalAddrSize = resourceHvaSize,
                .virtioGpuContextId = ctxId,
                .virtioGpuCapsetId = context.capsetId,
                .contextName = name.c_str(),
                .contextNameSize = static_cast<uint32_t>(context.name.size()),
            };

            mAddressSpaceDeviceControlOps->create_instance(createInfo);
            if (setContextAddressSpaceHandleLocked(ctxId, handle, contextCreate.resourceId)) {
                return -EINVAL;
            }
            break;
        }
        case GFXSTREAM_CONTEXT_PING: {
            DECODE(contextPing, gfxstream::gfxstreamContextPing, dwords)

            struct android::emulation::AddressSpaceDevicePingInfo ping = {0};
            ping.metadata = ASG_NOTIFY_AVAILABLE;

            mAddressSpaceDeviceControlOps->ping_at_hva(
                getAddressSpaceHandleLocked(ctxId, contextPing.resourceId), &ping);
            break;
        }
        default:
            break;
    }

    return 0;
}

int VirtioGpuFrontend::submitCmd(struct stream_renderer_command* cmd) {
    if (!cmd) return -EINVAL;

    void* buffer = reinterpret_cast<void*>(cmd->cmd);

    VirtioGpuRing ring = VirtioGpuRingGlobal{};
    stream_renderer_debug("ctx: % u, ring: %s buffer: %p dwords: %d", cmd->ctx_id,
                          to_string(ring).c_str(), buffer, cmd->cmd_size);

    if (!buffer) {
        stream_renderer_error("error: buffer null");
        return -EINVAL;
    }

    if (cmd->cmd_size < 4) {
        stream_renderer_error("error: not enough bytes (got %d)", cmd->cmd_size);
        return -EINVAL;
    }

    DECODE(header, gfxstream::gfxstreamHeader, buffer);
    switch (header.opCode) {
        case GFXSTREAM_CONTEXT_CREATE:
        case GFXSTREAM_CONTEXT_PING:
        case GFXSTREAM_CONTEXT_PING_WITH_RESPONSE: {
            GFXSTREAM_TRACE_EVENT(GFXSTREAM_TRACE_STREAM_RENDERER_CATEGORY,
                                  "GFXSTREAM_CONTEXT_[CREATE|PING]");

            if (addressSpaceProcessCmd(cmd->ctx_id, (uint32_t*)buffer)) {
                return -EINVAL;
            }
            break;
        }
        case GFXSTREAM_CREATE_EXPORT_SYNC: {
            GFXSTREAM_TRACE_EVENT(GFXSTREAM_TRACE_STREAM_RENDERER_CATEGORY,
                                  "GFXSTREAM_CREATE_EXPORT_SYNC");

            DECODE(exportSync, gfxstream::gfxstreamCreateExportSync, buffer)

            uint64_t sync_handle = convert32to64(exportSync.syncHandleLo, exportSync.syncHandleHi);

            stream_renderer_debug("wait for gpu ring %s", to_string(ring).c_str());
            auto taskId = mVirtioGpuTimelines->enqueueTask(ring);
#if GFXSTREAM_ENABLE_HOST_GLES
            gfxstream::FrameBuffer::getFB()->asyncWaitForGpuWithCb(
                sync_handle, [this, taskId] { mVirtioGpuTimelines->notifyTaskCompletion(taskId); });
#endif
            break;
        }
        case GFXSTREAM_CREATE_EXPORT_SYNC_VK:
        case GFXSTREAM_CREATE_IMPORT_SYNC_VK: {
            GFXSTREAM_TRACE_EVENT(GFXSTREAM_TRACE_STREAM_RENDERER_CATEGORY,
                                  "GFXSTREAM_CREATE_[IMPORT|EXPORT]_SYNC_VK");

            // The guest sync export assumes fence context support and always uses
            // VIRTGPU_EXECBUF_RING_IDX. With this, the task created here must use
            // the same ring as the fence created for the virtio gpu command or the
            // fence may be signaled without properly waiting for the task to complete.
            ring = VirtioGpuRingContextSpecific{
                .mCtxId = cmd->ctx_id,
                .mRingIdx = 0,
            };

            DECODE(exportSyncVK, gfxstream::gfxstreamCreateExportSyncVK, buffer)

            uint64_t device_handle =
                convert32to64(exportSyncVK.deviceHandleLo, exportSyncVK.deviceHandleHi);

            uint64_t fence_handle =
                convert32to64(exportSyncVK.fenceHandleLo, exportSyncVK.fenceHandleHi);

            stream_renderer_debug("wait for gpu ring %s", to_string(ring).c_str());
            auto taskId = mVirtioGpuTimelines->enqueueTask(ring);
            gfxstream::FrameBuffer::getFB()->asyncWaitForGpuVulkanWithCb(
                device_handle, fence_handle,
                [this, taskId] { mVirtioGpuTimelines->notifyTaskCompletion(taskId); });
            break;
        }
        case GFXSTREAM_CREATE_QSRI_EXPORT_VK: {
            GFXSTREAM_TRACE_EVENT(GFXSTREAM_TRACE_STREAM_RENDERER_CATEGORY,
                                  "GFXSTREAM_CREATE_QSRI_EXPORT_VK");

            // The guest QSRI export assumes fence context support and always uses
            // VIRTGPU_EXECBUF_RING_IDX. With this, the task created here must use
            // the same ring as the fence created for the virtio gpu command or the
            // fence may be signaled without properly waiting for the task to complete.
            ring = VirtioGpuRingContextSpecific{
                .mCtxId = cmd->ctx_id,
                .mRingIdx = 0,
            };

            DECODE(exportQSRI, gfxstream::gfxstreamCreateQSRIExportVK, buffer)

            uint64_t image_handle =
                convert32to64(exportQSRI.imageHandleLo, exportQSRI.imageHandleHi);

            stream_renderer_debug("wait for gpu vk qsri ring %u image 0x%llx",
                                  to_string(ring).c_str(), (unsigned long long)image_handle);
            auto taskId = mVirtioGpuTimelines->enqueueTask(ring);
            gfxstream::FrameBuffer::getFB()->asyncWaitForGpuVulkanQsriWithCb(
                image_handle,
                [this, taskId] { mVirtioGpuTimelines->notifyTaskCompletion(taskId); });
            break;
        }
        case GFXSTREAM_RESOURCE_CREATE_3D: {
            GFXSTREAM_TRACE_EVENT(GFXSTREAM_TRACE_STREAM_RENDERER_CATEGORY,
                                  "GFXSTREAM_RESOURCE_CREATE_3D");

            DECODE(create3d, gfxstream::gfxstreamResourceCreate3d, buffer)
            struct stream_renderer_resource_create_args rc3d = {0};

            rc3d.target = create3d.target;
            rc3d.format = create3d.format;
            rc3d.bind = create3d.bind;
            rc3d.width = create3d.width;
            rc3d.height = create3d.height;
            rc3d.depth = create3d.depth;
            rc3d.array_size = create3d.arraySize;
            rc3d.last_level = create3d.lastLevel;
            rc3d.nr_samples = create3d.nrSamples;
            rc3d.flags = create3d.flags;

            auto ctxIt = mContexts.find(cmd->ctx_id);
            if (ctxIt == mContexts.end()) {
                stream_renderer_error("ctx id %u is not found", cmd->ctx_id);
                return -EINVAL;
            }

            auto& ctxEntry = ctxIt->second;
            if (ctxEntry.blobMap.count(create3d.blobId)) {
                stream_renderer_error("blob ID already in use");
                return -EINVAL;
            }

            ctxEntry.blobMap[create3d.blobId] = rc3d;
            break;
        }
        case GFXSTREAM_ACQUIRE_SYNC: {
            GFXSTREAM_TRACE_EVENT(GFXSTREAM_TRACE_STREAM_RENDERER_CATEGORY,
                                  "GFXSTREAM_ACQUIRE_SYNC");

            DECODE(acquireSync, gfxstream::gfxstreamAcquireSync, buffer);

            auto ctxIt = mContexts.find(cmd->ctx_id);
            if (ctxIt == mContexts.end()) {
                stream_renderer_error("ctx id %u is not found", cmd->ctx_id);
                return -EINVAL;
            }

            auto& ctxEntry = ctxIt->second;
            if (ctxEntry.latestFence) {
                stream_renderer_error("expected latest fence to empty");
                return -EINVAL;
            }

            auto syncDescriptorInfoOpt = ExternalObjectManager::get()->removeSyncDescriptorInfo(
                cmd->ctx_id, acquireSync.syncId);
            if (syncDescriptorInfoOpt) {
                ctxEntry.latestFence = std::make_shared<gfxstream::SyncDescriptorInfo>(
                    std::move(*syncDescriptorInfoOpt));
            } else {
                stream_renderer_error("failed to get sync descriptor info");
                return -EINVAL;
            }

            break;
        }
        case GFXSTREAM_PLACEHOLDER_COMMAND_VK: {
            GFXSTREAM_TRACE_EVENT(GFXSTREAM_TRACE_STREAM_RENDERER_CATEGORY,
                                  "GFXSTREAM_PLACEHOLDER_COMMAND_VK");

            // Do nothing, this is a placeholder command
            break;
        }
        default:
            return -EINVAL;
    }

    return 0;
}

int VirtioGpuFrontend::createFence(uint64_t fence_id, const VirtioGpuRing& ring) {
    stream_renderer_debug("fenceid: %llu ring: %s", (unsigned long long)fence_id,
                          to_string(ring).c_str());

    struct {
        FenceCompletionCallback operator()(const VirtioGpuRingGlobal&) {
            return [frontend = mFrontend, fenceId = mFenceId] {
                struct stream_renderer_fence fence = {0};
                fence.fence_id = fenceId;
                fence.flags = STREAM_RENDERER_FLAG_FENCE;
                frontend->mFenceCallback(frontend->mCookie, &fence);
            };
        }
        FenceCompletionCallback operator()(const VirtioGpuRingContextSpecific& ring) {
            return [frontend = mFrontend, fenceId = mFenceId, ring] {
                struct stream_renderer_fence fence = {0};
                fence.fence_id = fenceId;
                fence.flags = STREAM_RENDERER_FLAG_FENCE | STREAM_RENDERER_FLAG_FENCE_RING_IDX;
                fence.ctx_id = ring.mCtxId;
                fence.ring_idx = ring.mRingIdx;
                frontend->mFenceCallback(frontend->mCookie, &fence);
            };
        }

        VirtioGpuFrontend* mFrontend;
        VirtioGpuTimelines::FenceId mFenceId;
    } visitor{
        .mFrontend = this,
        .mFenceId = fence_id,
    };
    FenceCompletionCallback callback = std::visit(visitor, ring);
    if (!callback) {
        return -EINVAL;
    }
    mVirtioGpuTimelines->enqueueFence(ring, fence_id, std::move(callback));

    return 0;
}

int VirtioGpuFrontend::acquireContextFence(uint32_t ctx_id, uint64_t fenceId) {
    auto ctxIt = mContexts.find(ctx_id);
    if (ctxIt == mContexts.end()) {
        stream_renderer_error("ctx id %u is not found", ctx_id);
        return -EINVAL;
    }

    auto& ctxEntry = ctxIt->second;
    if (ctxEntry.latestFence) {
        mSyncMap[fenceId] = ctxEntry.latestFence;
        ctxEntry.latestFence = nullptr;
    } else {
        stream_renderer_error("Failed to acquire sync descriptor");
        return -EINVAL;
    }

    return 0;
}

void VirtioGpuFrontend::poll() { mVirtioGpuTimelines->poll(); }

int VirtioGpuFrontend::createResource(struct stream_renderer_resource_create_args* args,
                                      struct iovec* iov, uint32_t num_iovs) {
    auto resourceOpt = VirtioGpuResource::Create(args, iov, num_iovs);
    if (!resourceOpt) {
        stream_renderer_error("Failed to create resource %u.", args->handle);
        return -EINVAL;
    }
    mResources[args->handle] = std::move(*resourceOpt);
    return 0;
}

void VirtioGpuFrontend::unrefResource(uint32_t toUnrefId) {
    stream_renderer_debug("handle: %u", toUnrefId);

    auto it = mResources.find(toUnrefId);
    if (it == mResources.end()) return;

    for (auto& ctxIdResources : mContextResources) {
        detachResourceLocked(ctxIdResources.first, toUnrefId);
    }

    auto& resource = it->second;

    resource.Destroy();

    mResources.erase(it);
}

int VirtioGpuFrontend::attachIov(int resourceId, struct iovec* iov, int num_iovs) {
    stream_renderer_debug("resource:%d numiovs: %d", resourceId, num_iovs);

    auto it = mResources.find(resourceId);
    if (it == mResources.end()) {
        stream_renderer_error("failed to attach iov: resource %u not found.", resourceId);
        return ENOENT;
    }
    auto& resource = it->second;
    resource.AttachIov(iov, num_iovs);
    return 0;
}

void VirtioGpuFrontend::detachIov(int resourceId) {
    stream_renderer_debug("resource:%d", resourceId);

    auto it = mResources.find(resourceId);
    if (it == mResources.end()) {
        stream_renderer_error("failed to detach iov: resource %u not found.", resourceId);
        return;
    }
    auto& resource = it->second;
    resource.DetachIov();
}

namespace {

std::optional<std::vector<struct iovec>> AsVecOption(struct iovec* iov, int iovec_cnt) {
    if (iovec_cnt > 0) {
        std::vector<struct iovec> ret;
        ret.reserve(iovec_cnt);
        for (int i = 0; i < iovec_cnt; i++) {
            ret.push_back(iov[i]);
        }
        return ret;
    }
    return std::nullopt;
}

}  // namespace

int VirtioGpuFrontend::transferReadIov(int resId, uint64_t offset, stream_renderer_box* box,
                                       struct iovec* iov, int iovec_cnt) {
    auto it = mResources.find(resId);
    if (it == mResources.end()) {
        stream_renderer_error("Failed to transfer: failed to find resource %d.", resId);
        return EINVAL;
    }
    auto& resource = it->second;

    auto ops = ensureAndGetServiceOps();
    return resource.TransferRead(ops, offset, box, AsVecOption(iov, iovec_cnt));
}

int VirtioGpuFrontend::transferWriteIov(int resId, uint64_t offset, stream_renderer_box* box,
                                        struct iovec* iov, int iovec_cnt) {
    auto it = mResources.find(resId);
    if (it == mResources.end()) {
        stream_renderer_error("Failed to transfer: failed to find resource %d.", resId);
        return EINVAL;
    }
    auto& resource = it->second;

    auto ops = ensureAndGetServiceOps();
    auto result = resource.TransferWrite(ops, offset, box, AsVecOption(iov, iovec_cnt));
    if (result.status != 0) return result.status;

    if (result.contextPipe) {
        resetPipe(result.contextId, result.contextPipe);
    }
    return 0;
}

void VirtioGpuFrontend::getCapset(uint32_t set, uint32_t* max_size) {
    switch (set) {
        case VIRTGPU_CAPSET_GFXSTREAM_VULKAN:
            *max_size = sizeof(struct gfxstream::vulkanCapset);
            break;
        case VIRTGPU_CAPSET_GFXSTREAM_MAGMA:
            *max_size = sizeof(struct gfxstream::magmaCapset);
            break;
        case VIRTGPU_CAPSET_GFXSTREAM_GLES:
            *max_size = sizeof(struct gfxstream::glesCapset);
            break;
        case VIRTGPU_CAPSET_GFXSTREAM_COMPOSER:
            *max_size = sizeof(struct gfxstream::composerCapset);
            break;
        default:
            stream_renderer_error("Incorrect capability set specified (%u)", set);
    }
}

void VirtioGpuFrontend::fillCaps(uint32_t set, void* caps) {
    switch (set) {
        case VIRTGPU_CAPSET_GFXSTREAM_VULKAN: {
            struct gfxstream::vulkanCapset* capset =
                reinterpret_cast<struct gfxstream::vulkanCapset*>(caps);

            memset(capset, 0, sizeof(*capset));

            capset->protocolVersion = 1;
            capset->ringSize = 12288;
            capset->bufferSize = 1048576;

            auto vk_emu = gfxstream::vk::getGlobalVkEmulation();
            if (vk_emu && vk_emu->live && vk_emu->representativeColorBufferMemoryTypeInfo) {
                capset->colorBufferMemoryIndex =
                    vk_emu->representativeColorBufferMemoryTypeInfo->guestMemoryTypeIndex;
            }

            if (mFeatures.VulkanBatchedDescriptorSetUpdate.enabled) {
                capset->vulkanBatchedDescriptorSetUpdate=1;
            }
            capset->noRenderControlEnc = 1;
            capset->blobAlignment = mPageSize;
            if (vk_emu && vk_emu->live) {
                capset->deferredMapping = 1;
            }

#if GFXSTREAM_UNSTABLE_VULKAN_DMABUF_WINSYS
            capset->alwaysBlob = 1;
#endif

#if GFXSTREAM_UNSTABLE_VULKAN_EXTERNAL_SYNC
            capset->externalSync = 1;
#endif

            memset(capset->virglSupportedFormats, 0, sizeof(capset->virglSupportedFormats));

            struct FormatWithName {
                uint32_t format;
                const char* name;
            };
#define MAKE_FORMAT_AND_NAME(x) \
    {                           \
        x, #x                   \
    }
            static const FormatWithName kPossibleFormats[] = {
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_B5G6R5_UNORM),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_B8G8R8A8_UNORM),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_B8G8R8X8_UNORM),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_NV12),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_P010),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_R10G10B10A2_UNORM),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_R16_UNORM),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_R16G16B16A16_FLOAT),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_R8_UNORM),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_R8G8_UNORM),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_R8G8B8_UNORM),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_R8G8B8A8_UNORM),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_R8G8B8X8_UNORM),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_YV12),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_Z16_UNORM),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_Z24_UNORM_S8_UINT),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_Z24X8_UNORM),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_Z32_FLOAT_S8X24_UINT),
                MAKE_FORMAT_AND_NAME(VIRGL_FORMAT_Z32_FLOAT),
            };
#undef MAKE_FORMAT_AND_NAME

            stream_renderer_info("Format support:");
            for (std::size_t i = 0; i < std::size(kPossibleFormats); i++) {
                const FormatWithName& possibleFormat = kPossibleFormats[i];

                GLenum possibleFormatGl = virgl_format_to_gl(possibleFormat.format);
                const bool supported =
                    gfxstream::FrameBuffer::getFB()->isFormatSupported(possibleFormatGl);

                stream_renderer_info(" %s: %s", possibleFormat.name,
                                     (supported ? "supported" : "unsupported"));
                set_virgl_format_supported(capset->virglSupportedFormats, possibleFormat.format,
                                           supported);
            }
            break;
        }
        case VIRTGPU_CAPSET_GFXSTREAM_MAGMA: {
            struct gfxstream::magmaCapset* capset =
                reinterpret_cast<struct gfxstream::magmaCapset*>(caps);

            capset->protocolVersion = 1;
            capset->ringSize = 12288;
            capset->bufferSize = 1048576;
            capset->blobAlignment = mPageSize;
            break;
        }
        case VIRTGPU_CAPSET_GFXSTREAM_GLES: {
            struct gfxstream::glesCapset* capset =
                reinterpret_cast<struct gfxstream::glesCapset*>(caps);

            capset->protocolVersion = 1;
            capset->ringSize = 12288;
            capset->bufferSize = 1048576;
            capset->blobAlignment = mPageSize;
            break;
        }
        case VIRTGPU_CAPSET_GFXSTREAM_COMPOSER: {
            struct gfxstream::composerCapset* capset =
                reinterpret_cast<struct gfxstream::composerCapset*>(caps);

            capset->protocolVersion = 1;
            capset->ringSize = 12288;
            capset->bufferSize = 1048576;
            capset->blobAlignment = mPageSize;
            break;
        }
        default:
            stream_renderer_error("Incorrect capability set specified");
    }
}

void VirtioGpuFrontend::attachResource(uint32_t ctxId, uint32_t resId) {
    stream_renderer_debug("ctxid: %u resid: %u", ctxId, resId);

    auto resourcesIt = mContextResources.find(ctxId);
    if (resourcesIt == mContextResources.end()) {
        std::vector<VirtioGpuResourceId> ids;
        ids.push_back(resId);
        mContextResources[ctxId] = ids;
    } else {
        auto& ids = resourcesIt->second;
        auto idIt = std::find(ids.begin(), ids.end(), resId);
        if (idIt == ids.end()) ids.push_back(resId);
    }

    // Associate the host pipe of the resource entry with the host pipe of
    // the context entry.  That is, the last context to call attachResource
    // wins if there is any conflict.
    auto ctxEntryIt = mContexts.find(ctxId);
    auto resEntryIt = mResources.find(resId);

    if (ctxEntryIt == mContexts.end() || resEntryIt == mResources.end()) return;

    stream_renderer_debug("hostPipe: %p", ctxEntryIt->second.hostPipe);
    resEntryIt->second.SetHostPipe(ctxEntryIt->second.hostPipe);
    resEntryIt->second.AttachToContext(ctxId);
}

void VirtioGpuFrontend::detachResource(uint32_t ctxId, uint32_t toUnrefId) {
    stream_renderer_debug("ctxid: %u resid: %u", ctxId, toUnrefId);
    detachResourceLocked(ctxId, toUnrefId);
}

int VirtioGpuFrontend::getResourceInfo(uint32_t resourceId,
                                       struct stream_renderer_resource_info* info) {
    stream_renderer_debug("resource: %u", resourceId);

    if (!info) {
        stream_renderer_error("Failed to get info: invalid info struct.");
        return EINVAL;
    }

    auto resourceIt = mResources.find(resourceId);
    if (resourceIt == mResources.end()) {
        stream_renderer_error("Failed to get info: failed to find resource %d.", resourceId);
        return ENOENT;
    }
    auto& resource = resourceIt->second;
    return resource.GetInfo(info);
}

void VirtioGpuFrontend::flushResource(uint32_t res_handle) {
    auto taskId = mVirtioGpuTimelines->enqueueTask(VirtioGpuRingGlobal{});
    gfxstream::FrameBuffer::getFB()->postWithCallback(
        res_handle, [this, taskId](std::shared_future<void> waitForGpu) {
            waitForGpu.wait();
            mVirtioGpuTimelines->notifyTaskCompletion(taskId);
        });
}

int VirtioGpuFrontend::createBlob(uint32_t contextId, uint32_t resourceId,
                                  const struct stream_renderer_create_blob* createBlobArgs,
                                  const struct stream_renderer_handle* handle) {
    auto contextIt = mContexts.find(contextId);
    if (contextIt == mContexts.end()) {
        stream_renderer_error("failed to create blob resource %u: context %u missing.", resourceId,
                              contextId);
        return -EINVAL;
    }
    auto& context = contextIt->second;

    std::optional<struct stream_renderer_resource_create_args> createArgs;

    auto blobIt = context.blobMap.find(createBlobArgs->blob_id);
    if (blobIt != context.blobMap.end()) {
        createArgs = blobIt->second;
        context.blobMap.erase(blobIt);
    }

    auto resourceOpt =
        VirtioGpuResource::Create(mFeatures, mPageSize, contextId, resourceId,
                                  createArgs ? &*createArgs : nullptr, createBlobArgs, handle);
    if (!resourceOpt) {
        stream_renderer_error("failed to create blob resource %u.", resourceId);
        return -EINVAL;
    }
    mResources[resourceId] = std::move(*resourceOpt);
    return 0;
}

int VirtioGpuFrontend::resourceMap(uint32_t resourceId, void** hvaOut, uint64_t* sizeOut) {
    stream_renderer_debug("resource: %u", resourceId);

    if (mFeatures.ExternalBlob.enabled) {
        stream_renderer_error("Failed to map resource: external blob enabled.");
        return -EINVAL;
    }

    auto it = mResources.find(resourceId);
    if (it == mResources.end()) {
        if (hvaOut) *hvaOut = nullptr;
        if (sizeOut) *sizeOut = 0;

        stream_renderer_error("Failed to map resource: unknown resource id %d.", resourceId);
        return -EINVAL;
    }

    auto& resource = it->second;
    return resource.Map(hvaOut, sizeOut);
}

int VirtioGpuFrontend::resourceUnmap(uint32_t resourceId) {
    stream_renderer_debug("resource: %u", resourceId);

    auto it = mResources.find(resourceId);
    if (it == mResources.end()) {
        stream_renderer_error("Failed to map resource: unknown resource id %d.", resourceId);
        return -EINVAL;
    }

    // TODO(lfy): Good place to run any registered cleanup callbacks.
    // No-op for now.
    return 0;
}

int VirtioGpuFrontend::platformImportResource(int res_handle, int res_info, void* resource) {
    auto it = mResources.find(res_handle);
    if (it == mResources.end()) return -EINVAL;
    bool success =
        gfxstream::FrameBuffer::getFB()->platformImportResource(res_handle, res_info, resource);
    return success ? 0 : -1;
}

void* VirtioGpuFrontend::platformCreateSharedEglContext() {
    void* ptr = nullptr;
#if GFXSTREAM_ENABLE_HOST_GLES
    ptr = gfxstream::FrameBuffer::getFB()->platformCreateSharedEglContext();
#endif
    return ptr;
}

int VirtioGpuFrontend::platformDestroySharedEglContext(void* context) {
    bool success = false;
#if GFXSTREAM_ENABLE_HOST_GLES
    success = gfxstream::FrameBuffer::getFB()->platformDestroySharedEglContext(context);
#endif
    return success ? 0 : -1;
}

int VirtioGpuFrontend::waitSyncResource(uint32_t res_handle) {
    auto resourceIt = mResources.find(res_handle);
    if (resourceIt == mResources.end()) {
        stream_renderer_error("waitSyncResource could not find resource: %d", res_handle);
        return -EINVAL;
    }
    auto& resource = resourceIt->second;
    return resource.WaitSyncResource();
}

int VirtioGpuFrontend::resourceMapInfo(uint32_t resourceId, uint32_t* map_info) {
    stream_renderer_debug("resource: %u", resourceId);

    auto resourceIt = mResources.find(resourceId);
    if (resourceIt == mResources.end()) {
        stream_renderer_error("Failed to get resource map info: unknown resource %d.", resourceId);
        return -EINVAL;
    }

    const auto& resource = resourceIt->second;
    return resource.GetCaching(map_info);
}

int VirtioGpuFrontend::exportBlob(uint32_t resourceId, struct stream_renderer_handle* handle) {
    stream_renderer_debug("resource: %u", resourceId);

    auto resourceIt = mResources.find(resourceId);
    if (resourceIt == mResources.end()) {
        stream_renderer_error("Failed to export blob: unknown resource %d.", resourceId);
        return -EINVAL;
    }
    auto& resource = resourceIt->second;
    return resource.ExportBlob(handle);
}

int VirtioGpuFrontend::exportFence(uint64_t fenceId, struct stream_renderer_handle* handle) {
    auto it = mSyncMap.find(fenceId);
    if (it == mSyncMap.end()) {
        return -EINVAL;
    }

    auto& entry = it->second;
    DescriptorType rawDescriptor;
    auto rawDescriptorOpt = entry->descriptor.release();
    if (rawDescriptorOpt)
        rawDescriptor = *rawDescriptorOpt;
    else
        return -EINVAL;

    handle->handle_type = entry->handleType;

#ifdef _WIN32
    handle->os_handle = static_cast<int64_t>(reinterpret_cast<intptr_t>(rawDescriptor));
#else
    handle->os_handle = static_cast<int64_t>(rawDescriptor);
#endif

    return 0;
}

int VirtioGpuFrontend::vulkanInfo(uint32_t resourceId,
                                  struct stream_renderer_vulkan_info* vulkanInfo) {
    auto resourceIt = mResources.find(resourceId);
    if (resourceIt == mResources.end()) {
        stream_renderer_error("failed to get vulkan info: failed to find resource %d", resourceId);
        return -EINVAL;
    }
    auto& resource = resourceIt->second;
    return resource.GetVulkanInfo(vulkanInfo);
}

#ifdef CONFIG_AEMU
void VirtioGpuFrontend::setServiceOps(const GoldfishPipeServiceOps* ops) { mServiceOps = ops; }
#endif  // CONFIG_AEMU

void VirtioGpuFrontend::detachResourceLocked(uint32_t ctxId, uint32_t toUnrefId) {
    stream_renderer_debug("ctxid: %u resid: %u", ctxId, toUnrefId);

    auto it = mContextResources.find(ctxId);
    if (it == mContextResources.end()) return;

    std::vector<VirtioGpuResourceId> withoutRes;
    for (auto resId : it->second) {
        if (resId != toUnrefId) {
            withoutRes.push_back(resId);
        }
    }
    mContextResources[ctxId] = withoutRes;

    auto resourceIt = mResources.find(toUnrefId);
    if (resourceIt == mResources.end()) return;
    auto& resource = resourceIt->second;
    resource.DetachFromContext();

    auto ctxIt = mContexts.find(ctxId);
    if (ctxIt != mContexts.end()) {
        auto& ctxEntry = ctxIt->second;
        if (ctxEntry.addressSpaceHandles.count(toUnrefId)) {
            uint32_t asgHandle = ctxEntry.addressSpaceHandles[toUnrefId];

            mCleanupThread->enqueueCleanup([this, asgBlob = resource.ShareRingBlob(), asgHandle]() {
                mAddressSpaceDeviceControlOps->destroy_handle(asgHandle);
            });

            ctxEntry.addressSpaceHandles.erase(toUnrefId);
        }
    }
}

inline const GoldfishPipeServiceOps* VirtioGpuFrontend::ensureAndGetServiceOps() {
    if (mServiceOps) return mServiceOps;
    mServiceOps = goldfish_pipe_get_service_ops();
    return mServiceOps;
}

#ifdef GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT

int VirtioGpuFrontend::snapshot(gfxstream::host::snapshot::VirtioGpuFrontendSnapshot& outSnapshot) {
    for (const auto& [contextId, context] : mContexts) {
        auto contextSnapshotOpt = SnapshotContext(context);
        if (!contextSnapshotOpt) {
            stream_renderer_error("Failed to snapshot context %d", contextId);
            return -1;
        }
        (*outSnapshot.mutable_contexts())[contextId] = std::move(*contextSnapshotOpt);
    }
    for (const auto& [resourceId, resource] : mResources) {
        auto resourceSnapshotOpt = resource.Snapshot();
        if (!resourceSnapshotOpt) {
            stream_renderer_error("Failed to snapshot resource %d", resourceId);
            return -1;
        }
        (*outSnapshot.mutable_resources())[resourceId] = std::move(*resourceSnapshotOpt);
    }
    return 0;
}

int VirtioGpuFrontend::restore(const VirtioGpuFrontendSnapshot& snapshot) {
    mContexts.clear();
    mResources.clear();
    for (const auto& [contextId, contextSnapshot] : snapshot.contexts()) {
        auto contextOpt = RestoreContext(contextSnapshot);
        if (!contextOpt) {
            stream_renderer_error("Failed to restore context %d", contextId);
            return -1;
        }
        mContexts.emplace(contextId, std::move(*contextOpt));
    }
    for (const auto& [resourceId, resourceSnapshot] : snapshot.resources()) {
        auto resourceOpt = VirtioGpuResource::Restore(resourceSnapshot);
        if (!resourceOpt) {
            stream_renderer_error("Failed to restore resource %d", resourceId);
            return -1;
        }
        mResources.emplace(resourceId, std::move(*resourceOpt));
    }
    return 0;
}

#endif  // ifdef GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT

}  // namespace host
}  // namespace gfxstream
