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

enum pipe_texture_target {
    PIPE_BUFFER,
    PIPE_TEXTURE_1D,
    PIPE_TEXTURE_2D,
    PIPE_TEXTURE_3D,
    PIPE_TEXTURE_CUBE,
    PIPE_TEXTURE_RECT,
    PIPE_TEXTURE_1D_ARRAY,
    PIPE_TEXTURE_2D_ARRAY,
    PIPE_TEXTURE_CUBE_ARRAY,
    PIPE_MAX_TEXTURE_TYPES,
};

/**
 *  Resource binding flags -- state tracker must specify in advance all
 *  the ways a resource might be used.
 */
#define PIPE_BIND_DEPTH_STENCIL (1 << 0)        /* create_surface */
#define PIPE_BIND_RENDER_TARGET (1 << 1)        /* create_surface */
#define PIPE_BIND_BLENDABLE (1 << 2)            /* create_surface */
#define PIPE_BIND_SAMPLER_VIEW (1 << 3)         /* create_sampler_view */
#define PIPE_BIND_VERTEX_BUFFER (1 << 4)        /* set_vertex_buffers */
#define PIPE_BIND_INDEX_BUFFER (1 << 5)         /* draw_elements */
#define PIPE_BIND_CONSTANT_BUFFER (1 << 6)      /* set_constant_buffer */
#define PIPE_BIND_DISPLAY_TARGET (1 << 7)       /* flush_front_buffer */
#define PIPE_BIND_STREAM_OUTPUT (1 << 10)       /* set_stream_output_buffers */
#define PIPE_BIND_CURSOR (1 << 11)              /* mouse cursor */
#define PIPE_BIND_CUSTOM (1 << 12)              /* state-tracker/winsys usages */
#define PIPE_BIND_GLOBAL (1 << 13)              /* set_global_binding */
#define PIPE_BIND_SHADER_BUFFER (1 << 14)       /* set_shader_buffers */
#define PIPE_BIND_SHADER_IMAGE (1 << 15)        /* set_shader_images */
#define PIPE_BIND_COMPUTE_RESOURCE (1 << 16)    /* set_compute_resources */
#define PIPE_BIND_COMMAND_ARGS_BUFFER (1 << 17) /* pipe_draw_info.indirect */
#define PIPE_BIND_QUERY_BUFFER (1 << 18)        /* get_query_result_resource */

static constexpr int kPipeTryAgain = -2;

struct VirtioGpuCmd {
    uint32_t op;
    uint32_t cmdSize;
    unsigned char buf[0];
} __attribute__((packed));

static inline uint32_t align_up(uint32_t n, uint32_t a) { return ((n + a - 1) / a) * a; }

enum IovSyncDir {
    IOV_TO_LINEAR = 0,
    LINEAR_TO_IOV = 1,
};

static int sync_iov(VirtioGpuResource* res, uint64_t offset, const stream_renderer_box* box,
                    IovSyncDir dir) {
    stream_renderer_debug("offset: 0x%llx box: %u %u %u %u size %u x %u iovs %u linearSize %zu",
                          (unsigned long long)offset, box->x, box->y, box->w, box->h,
                          res->args.width, res->args.height, res->numIovs, res->linearSize);

    if (box->x > res->args.width || box->y > res->args.height) {
        stream_renderer_error("Box out of range of resource");
        return -EINVAL;
    }
    if (box->w == 0U || box->h == 0U) {
        stream_renderer_error("Empty transfer");
        return -EINVAL;
    }
    if (box->x + box->w > res->args.width) {
        stream_renderer_error("Box overflows resource width");
        return -EINVAL;
    }

    size_t linearBase = virgl_format_to_linear_base(
        res->args.format, res->args.width, res->args.height, box->x, box->y, box->w, box->h);
    size_t start = linearBase;
    // height - 1 in order to treat the (w * bpp) row specially
    // (i.e., the last row does not occupy the full stride)
    size_t length = virgl_format_to_total_xfer_len(
        res->args.format, res->args.width, res->args.height, box->x, box->y, box->w, box->h);
    size_t end = start + length;

    if (start == end) {
        stream_renderer_error("nothing to transfer");
        return -EINVAL;
    }

    if (end > res->linearSize) {
        stream_renderer_error("start + length overflows!");
        return -EINVAL;
    }

    uint32_t iovIndex = 0;
    size_t iovOffset = 0;
    size_t written = 0;
    char* linear = static_cast<char*>(res->linear);

    while (written < length) {
        if (iovIndex >= res->numIovs) {
            stream_renderer_error("write request overflowed numIovs");
            return -EINVAL;
        }

        const char* iovBase_const = static_cast<const char*>(res->iov[iovIndex].iov_base);
        char* iovBase = static_cast<char*>(res->iov[iovIndex].iov_base);
        size_t iovLen = res->iov[iovIndex].iov_len;
        size_t iovOffsetEnd = iovOffset + iovLen;

        auto lower_intersect = std::max(iovOffset, start);
        auto upper_intersect = std::min(iovOffsetEnd, end);
        if (lower_intersect < upper_intersect) {
            size_t toWrite = upper_intersect - lower_intersect;
            switch (dir) {
                case IOV_TO_LINEAR:
                    memcpy(linear + lower_intersect, iovBase_const + lower_intersect - iovOffset,
                           toWrite);
                    break;
                case LINEAR_TO_IOV:
                    memcpy(iovBase + lower_intersect - iovOffset, linear + lower_intersect,
                           toWrite);
                    break;
                default:
                    stream_renderer_error("Invalid synchronization dir");
                    return -EINVAL;
            }
            written += toWrite;
        }
        ++iovIndex;
        iovOffset += iovLen;
    }

    return 0;
}

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

int VirtioGpuFrontend::resetPipe(GoldfishHwPipe* hwPipe, GoldfishHostPipe* hostPipe) {
    stream_renderer_debug("Want to reset hwpipe %p to hostpipe %p", hwPipe, hostPipe);
    VirtioGpuCtxId asCtxId = (VirtioGpuCtxId)(uintptr_t)hwPipe;
    auto it = mContexts.find(asCtxId);
    if (it == mContexts.end()) {
        stream_renderer_error("fatal: pipe id %u", asCtxId);
        return -EINVAL;
    }

    auto& entry = it->second;
    stream_renderer_debug("ctxid: %u prev hostpipe: %p", asCtxId, entry.hostPipe);
    entry.hostPipe = hostPipe;
    stream_renderer_debug("ctxid: %u next hostpipe: %p", asCtxId, entry.hostPipe);

    // Also update any resources associated with it
    auto resourcesIt = mContextResources.find(asCtxId);

    if (resourcesIt == mContextResources.end()) {
        return 0;
    }

    const auto& resIds = resourcesIt->second;

    for (auto resId : resIds) {
        auto resEntryIt = mResources.find(resId);
        if (resEntryIt == mResources.end()) {
            stream_renderer_error("entry with res id %u not found", resId);
            return -EINVAL;
        }

        auto& resEntry = resEntryIt->second;
        resEntry.hostPipe = hostPipe;
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
        0,                       // fence
        0,                       // AS handle
        false,                   // does not have an AS handle
        map,                     // resourceId --> ASG handle map
        blobMap,                 // blobId -> resource create args
    };

    stream_renderer_debug("initial host pipe for ctxid %u: %p", ctx_id, hostPipe);
    mContexts[ctx_id] = res;
    android_onGuestGraphicsProcessCreate(ctx_id);
    return 0;
}

int VirtioGpuFrontend::destroyContext(VirtioGpuCtxId handle) {
    stream_renderer_debug("ctxid: %u", handle);

    auto it = mContexts.find(handle);
    if (it == mContexts.end()) {
        stream_renderer_error("could not find context handle %u", handle);
        return -EINVAL;
    }

    if (it->second.hasAddressSpaceHandle) {
        for (auto const& [resourceId, handle] : it->second.addressSpaceHandles) {
            // Note: this can hang as is but this has only been observed to
            // happen during shutdown. See b/329287602#comment8.
            mAddressSpaceDeviceControlOps->destroy_handle(handle);
        }
    }

    auto hostPipe = it->second.hostPipe;
    if (!hostPipe) {
        stream_renderer_error("0 is not a valid hostpipe");
        return -EINVAL;
    }

    auto ops = ensureAndGetServiceOps();
    ops->guest_close(hostPipe, GOLDFISH_PIPE_CLOSE_GRACEFUL);

    android_cleanupProcGLObjects(handle);
    mContexts.erase(it);
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
    ctxEntry.addressSpaceHandle = handle;
    ctxEntry.hasAddressSpaceHandle = true;
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

            auto resEntryIt = mResources.find(contextCreate.resourceId);
            if (resEntryIt == mResources.end()) {
                stream_renderer_error("ASG coherent resource %u not found",
                                      contextCreate.resourceId);
                return -EINVAL;
            }

            auto ctxIt = mContexts.find(ctxId);
            if (ctxIt == mContexts.end()) {
                stream_renderer_error("ctx id %u not found", ctxId);
                return -EINVAL;
            }

            auto& ctxEntry = ctxIt->second;
            auto& resEntry = resEntryIt->second;

            std::string name = ctxEntry.name + "-" + std::to_string(contextCreate.resourceId);

            // Note: resource ids can not be used as ASG handles because ASGs may outlive the
            // containing resource due asynchronous ASG destruction.
            uint32_t handle = mAddressSpaceDeviceControlOps->gen_handle();

            struct AddressSpaceCreateInfo createInfo = {
                .handle = handle,
                .type = android::emulation::VirtioGpuGraphics,
                .createRenderThread = true,
                .externalAddr = resEntry.hva,
                .externalAddrSize = resEntry.hvaSize,
                .virtioGpuContextId = ctxId,
                .virtioGpuCapsetId = ctxEntry.capsetId,
                .contextName = name.c_str(),
                .contextNameSize = static_cast<uint32_t>(ctxEntry.name.size()),
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

VirtioGpuResourceType VirtioGpuFrontend::getResourceType(
    const struct stream_renderer_resource_create_args& args) const {
    if (args.target == PIPE_BUFFER) {
        return VirtioGpuResourceType::PIPE;
    }

    if (args.format != VIRGL_FORMAT_R8_UNORM) {
        return VirtioGpuResourceType::COLOR_BUFFER;
    }
    if (args.bind & VIRGL_BIND_SAMPLER_VIEW) {
        return VirtioGpuResourceType::COLOR_BUFFER;
    }
    if (args.bind & VIRGL_BIND_RENDER_TARGET) {
        return VirtioGpuResourceType::COLOR_BUFFER;
    }
    if (args.bind & VIRGL_BIND_SCANOUT) {
        return VirtioGpuResourceType::COLOR_BUFFER;
    }
    if (args.bind & VIRGL_BIND_CURSOR) {
        return VirtioGpuResourceType::COLOR_BUFFER;
    }
    if (!(args.bind & VIRGL_BIND_LINEAR)) {
        return VirtioGpuResourceType::COLOR_BUFFER;
    }

    return VirtioGpuResourceType::BUFFER;
}

void VirtioGpuFrontend::handleCreateResourceBuffer(
    struct stream_renderer_resource_create_args* args) {
    stream_renderer_debug("w:%u h:%u handle:%u", args->handle, args->width, args->height);
    gfxstream::FrameBuffer::getFB()->createBufferWithHandle(args->width * args->height,
                                                            args->handle);
}

void VirtioGpuFrontend::handleCreateResourceColorBuffer(
    struct stream_renderer_resource_create_args* args) {
    stream_renderer_debug("w h %u %u resid %u -> CreateColorBufferWithHandle", args->width,
                          args->height, args->handle);

    const uint32_t glformat = virgl_format_to_gl(args->format);
    const uint32_t fwkformat = virgl_format_to_fwk_format(args->format);

    const bool linear =
#ifdef GFXSTREAM_ENABLE_GUEST_VIRTIO_RESOURCE_TILING_CONTROL
        !!(args->bind & VIRGL_BIND_LINEAR);
#else
        false;
#endif
    gfxstream::FrameBuffer::getFB()->createColorBufferWithHandle(
        args->width, args->height, glformat, (gfxstream::FrameworkFormat)fwkformat, args->handle,
        linear);
    gfxstream::FrameBuffer::getFB()->setGuestManagedColorBufferLifetime(
        true /* guest manages lifetime */);
    gfxstream::FrameBuffer::getFB()->openColorBuffer(args->handle);
}

int VirtioGpuFrontend::createResource(struct stream_renderer_resource_create_args* args,
                                      struct iovec* iov, uint32_t num_iovs) {
    stream_renderer_debug("handle: %u. num iovs: %u", args->handle, num_iovs);

    const auto VirtioGpuResourceType = getResourceType(*args);
    switch (VirtioGpuResourceType) {
        case VirtioGpuResourceType::BLOB:
            return -EINVAL;
        case VirtioGpuResourceType::PIPE:
            break;
        case VirtioGpuResourceType::BUFFER:
            handleCreateResourceBuffer(args);
            break;
        case VirtioGpuResourceType::COLOR_BUFFER:
            handleCreateResourceColorBuffer(args);
            break;
    }

    VirtioGpuResource e;
    e.args = *args;
    e.linear = 0;
    e.hostPipe = 0;
    e.hva = nullptr;
    e.hvaSize = 0;
    e.blobId = 0;
    e.blobMem = 0;
    e.type = VirtioGpuResourceType;
    allocResource(e, iov, num_iovs);

    mResources[args->handle] = e;
    return 0;
}

void VirtioGpuFrontend::unrefResource(uint32_t toUnrefId) {
    stream_renderer_debug("handle: %u", toUnrefId);

    auto it = mResources.find(toUnrefId);
    if (it == mResources.end()) return;

    auto contextsIt = mResourceContexts.find(toUnrefId);
    if (contextsIt != mResourceContexts.end()) {
        mResourceContexts.erase(contextsIt->first);
    }

    for (auto& ctxIdResources : mContextResources) {
        detachResourceLocked(ctxIdResources.first, toUnrefId);
    }

    auto& entry = it->second;
    switch (entry.type) {
        case VirtioGpuResourceType::BLOB:
        case VirtioGpuResourceType::PIPE:
            break;
        case VirtioGpuResourceType::BUFFER:
            gfxstream::FrameBuffer::getFB()->closeBuffer(toUnrefId);
            break;
        case VirtioGpuResourceType::COLOR_BUFFER:
            gfxstream::FrameBuffer::getFB()->closeColorBuffer(toUnrefId);
            break;
    }

    if (entry.linear) {
        free(entry.linear);
        entry.linear = nullptr;
    }

    if (entry.iov) {
        free(entry.iov);
        entry.iov = nullptr;
        entry.numIovs = 0;
    }

    entry.hva = nullptr;
    entry.hvaSize = 0;
    entry.blobId = 0;

    mResources.erase(it);
}

int VirtioGpuFrontend::attachIov(int resId, iovec* iov, int num_iovs) {
    stream_renderer_debug("resid: %d numiovs: %d", resId, num_iovs);

    auto it = mResources.find(resId);
    if (it == mResources.end()) return ENOENT;

    auto& entry = it->second;
    stream_renderer_debug("res linear: %p", entry.linear);
    if (!entry.linear) allocResource(entry, iov, num_iovs);

    stream_renderer_debug("done");
    return 0;
}

void VirtioGpuFrontend::detachIov(int resId, iovec** iov, int* num_iovs) {
    auto it = mResources.find(resId);
    if (it == mResources.end()) return;

    auto& entry = it->second;

    if (num_iovs) {
        *num_iovs = entry.numIovs;
        stream_renderer_debug("resid: %d numIovs: %d", resId, *num_iovs);
    } else {
        stream_renderer_debug("resid: %d numIovs: 0", resId);
    }

    entry.numIovs = 0;

    if (entry.iov) free(entry.iov);
    entry.iov = nullptr;

    if (iov) {
        *iov = entry.iov;
    }

    allocResource(entry, entry.iov, entry.numIovs);
    stream_renderer_debug("done");
}

int VirtioGpuFrontend::handleTransferReadPipe(VirtioGpuResource* res, uint64_t offset,
                                              stream_renderer_box* box) {
    if (res->type != VirtioGpuResourceType::PIPE) {
        stream_renderer_error("resid: %d not a PIPE resource", res->args.handle);
        return -EINVAL;
    }

    // Do the pipe service op here, if there is an associated hostpipe.
    auto hostPipe = res->hostPipe;
    if (!hostPipe) return -EINVAL;

    auto ops = ensureAndGetServiceOps();

    size_t readBytes = 0;
    size_t wantedBytes = readBytes + (size_t)box->w;

    while (readBytes < wantedBytes) {
        GoldfishPipeBuffer buf = {
            ((char*)res->linear) + box->x + readBytes,
            wantedBytes - readBytes,
        };
        auto status = ops->guest_recv(hostPipe, &buf, 1);

        if (status > 0) {
            readBytes += status;
        } else if (status == kPipeTryAgain) {
            ops->wait_guest_recv(hostPipe);
        } else {
            return EIO;
        }
    }

    return 0;
}

int VirtioGpuFrontend::handleTransferWritePipe(VirtioGpuResource* res, uint64_t offset,
                                               stream_renderer_box* box) {
    if (res->type != VirtioGpuResourceType::PIPE) {
        stream_renderer_error("resid: %d not a PIPE resource", res->args.handle);
        return -EINVAL;
    }

    // Do the pipe service op here, if there is an associated hostpipe.
    auto hostPipe = res->hostPipe;
    if (!hostPipe) {
        stream_renderer_error("No hostPipe");
        return -EINVAL;
    }

    stream_renderer_debug("resid: %d offset: 0x%llx hostpipe: %p", res->args.handle,
                          (unsigned long long)offset, hostPipe);

    auto ops = ensureAndGetServiceOps();

    size_t writtenBytes = 0;
    size_t wantedBytes = (size_t)box->w;

    while (writtenBytes < wantedBytes) {
        GoldfishPipeBuffer buf = {
            ((char*)res->linear) + box->x + writtenBytes,
            wantedBytes - writtenBytes,
        };

        // guest_send can now reallocate the pipe.
        void* hostPipeBefore = hostPipe;
        auto status = ops->guest_send(&hostPipe, &buf, 1);
        if (hostPipe != hostPipeBefore) {
            if (resetPipe((GoldfishHwPipe*)(uintptr_t)(res->ctxId), hostPipe)) {
                return -EINVAL;
            }

            auto it = mResources.find(res->args.handle);
            res = &it->second;
        }

        if (status > 0) {
            writtenBytes += status;
        } else if (status == kPipeTryAgain) {
            ops->wait_guest_send(hostPipe);
        } else {
            return EIO;
        }
    }

    return 0;
}

int VirtioGpuFrontend::handleTransferReadBuffer(VirtioGpuResource* res, uint64_t offset,
                                                stream_renderer_box* box) {
    if (res->type != VirtioGpuResourceType::BUFFER) {
        stream_renderer_error("resid: %d not a BUFFER resource", res->args.handle);
        return -EINVAL;
    }

    gfxstream::FrameBuffer::getFB()->readBuffer(res->args.handle, 0,
                                                res->args.width * res->args.height, res->linear);
    return 0;
}

int VirtioGpuFrontend::handleTransferWriteBuffer(VirtioGpuResource* res, uint64_t offset,
                                                 stream_renderer_box* box) {
    if (res->type != VirtioGpuResourceType::BUFFER) {
        stream_renderer_error("resid: %d not a BUFFER resource", res->args.handle);
        return -EINVAL;
    }

    gfxstream::FrameBuffer::getFB()->updateBuffer(res->args.handle, 0,
                                                  res->args.width * res->args.height, res->linear);
    return 0;
}

int VirtioGpuFrontend::handleTransferReadColorBuffer(VirtioGpuResource* res, uint64_t offset,
                                                     stream_renderer_box* box) {
    if (res->type != VirtioGpuResourceType::COLOR_BUFFER) {
        stream_renderer_error("resid: %d not a COLOR_BUFFER resource", res->args.handle);
        return -EINVAL;
    }

    auto glformat = virgl_format_to_gl(res->args.format);
    auto gltype = gl_format_to_natural_type(glformat);

    // We always xfer the whole thing again from GL
    // since it's fiddly to calc / copy-out subregions
    if (virgl_format_is_yuv(res->args.format)) {
        gfxstream::FrameBuffer::getFB()->readColorBufferYUV(res->args.handle, 0, 0, res->args.width,
                                                            res->args.height, res->linear,
                                                            res->linearSize);
    } else {
        gfxstream::FrameBuffer::getFB()->readColorBuffer(res->args.handle, 0, 0, res->args.width,
                                                         res->args.height, glformat, gltype,
                                                         res->linear);
    }

    return 0;
}

int VirtioGpuFrontend::handleTransferWriteColorBuffer(VirtioGpuResource* res, uint64_t offset,
                                                      stream_renderer_box* box) {
    if (res->type != VirtioGpuResourceType::COLOR_BUFFER) {
        stream_renderer_error("resid: %d not a COLOR_BUFFER resource", res->args.handle);
        return -EINVAL;
    }

    auto glformat = virgl_format_to_gl(res->args.format);
    auto gltype = gl_format_to_natural_type(glformat);

    // We always xfer the whole thing again to GL
    // since it's fiddly to calc / copy-out subregions
    gfxstream::FrameBuffer::getFB()->updateColorBuffer(
        res->args.handle, 0, 0, res->args.width, res->args.height, glformat, gltype, res->linear);
    return 0;
}

int VirtioGpuFrontend::transferReadIov(int resId, uint64_t offset, stream_renderer_box* box,
                                       struct iovec* iov, int iovec_cnt) {
    auto it = mResources.find(resId);
    if (it == mResources.end()) return EINVAL;

    int ret = 0;

    auto& entry = it->second;
    switch (entry.type) {
        case VirtioGpuResourceType::BLOB:
            return -EINVAL;
        case VirtioGpuResourceType::PIPE:
            ret = handleTransferReadPipe(&entry, offset, box);
            break;
        case VirtioGpuResourceType::BUFFER:
            ret = handleTransferReadBuffer(&entry, offset, box);
            break;
        case VirtioGpuResourceType::COLOR_BUFFER:
            ret = handleTransferReadColorBuffer(&entry, offset, box);
            break;
    }

    if (ret != 0) {
        return ret;
    }

    if (iovec_cnt) {
        VirtioGpuResource e = {
            entry.args, iov, (uint32_t)iovec_cnt, entry.linear, entry.linearSize,
        };
        ret = sync_iov(&e, offset, box, LINEAR_TO_IOV);
    } else {
        ret = sync_iov(&entry, offset, box, LINEAR_TO_IOV);
    }

    return ret;
}

int VirtioGpuFrontend::transferWriteIov(int resId, uint64_t offset, stream_renderer_box* box,
                                        struct iovec* iov, int iovec_cnt) {
    auto it = mResources.find(resId);
    if (it == mResources.end()) return EINVAL;

    auto& entry = it->second;

    int ret = 0;
    if (iovec_cnt) {
        VirtioGpuResource e = {
            entry.args, iov, (uint32_t)iovec_cnt, entry.linear, entry.linearSize,
        };
        ret = sync_iov(&e, offset, box, IOV_TO_LINEAR);
    } else {
        ret = sync_iov(&entry, offset, box, IOV_TO_LINEAR);
    }

    if (ret != 0) {
        return ret;
    }

    switch (entry.type) {
        case VirtioGpuResourceType::BLOB:
            return -EINVAL;
        case VirtioGpuResourceType::PIPE:
            ret = handleTransferWritePipe(&entry, offset, box);
            break;
        case VirtioGpuResourceType::BUFFER:
            ret = handleTransferWriteBuffer(&entry, offset, box);
            break;
        case VirtioGpuResourceType::COLOR_BUFFER:
            ret = handleTransferWriteColorBuffer(&entry, offset, box);
            break;
    }

    return ret;
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

    auto contextsIt = mResourceContexts.find(resId);

    if (contextsIt == mResourceContexts.end()) {
        std::vector<VirtioGpuCtxId> ids;
        ids.push_back(ctxId);
        mResourceContexts[resId] = ids;
    } else {
        auto& ids = contextsIt->second;
        auto idIt = std::find(ids.begin(), ids.end(), ctxId);
        if (idIt == ids.end()) ids.push_back(ctxId);
    }

    // Associate the host pipe of the resource entry with the host pipe of
    // the context entry.  That is, the last context to call attachResource
    // wins if there is any conflict.
    auto ctxEntryIt = mContexts.find(ctxId);
    auto resEntryIt = mResources.find(resId);

    if (ctxEntryIt == mContexts.end() || resEntryIt == mResources.end()) return;

    stream_renderer_debug("hostPipe: %p", ctxEntryIt->second.hostPipe);
    resEntryIt->second.hostPipe = ctxEntryIt->second.hostPipe;
    resEntryIt->second.ctxId = ctxId;
}

void VirtioGpuFrontend::detachResource(uint32_t ctxId, uint32_t toUnrefId) {
    stream_renderer_debug("ctxid: %u resid: %u", ctxId, toUnrefId);
    detachResourceLocked(ctxId, toUnrefId);
}

int VirtioGpuFrontend::getResourceInfo(uint32_t resId, struct stream_renderer_resource_info* info) {
    stream_renderer_debug("resid: %u", resId);
    if (!info) return EINVAL;

    auto it = mResources.find(resId);
    if (it == mResources.end()) return ENOENT;

    auto& entry = it->second;

    uint32_t bpp = 4U;
    switch (entry.args.format) {
        case VIRGL_FORMAT_B8G8R8A8_UNORM:
            info->drm_fourcc = DRM_FORMAT_ARGB8888;
            break;
        case VIRGL_FORMAT_B8G8R8X8_UNORM:
            info->drm_fourcc = DRM_FORMAT_XRGB8888;
            break;
        case VIRGL_FORMAT_B5G6R5_UNORM:
            info->drm_fourcc = DRM_FORMAT_RGB565;
            bpp = 2U;
            break;
        case VIRGL_FORMAT_R8G8B8A8_UNORM:
            info->drm_fourcc = DRM_FORMAT_ABGR8888;
            break;
        case VIRGL_FORMAT_R8G8B8X8_UNORM:
            info->drm_fourcc = DRM_FORMAT_XBGR8888;
            break;
        case VIRGL_FORMAT_R8_UNORM:
            info->drm_fourcc = DRM_FORMAT_R8;
            bpp = 1U;
            break;
        default:
            return EINVAL;
    }

    info->stride = align_up(entry.args.width * bpp, 16U);
    info->virgl_format = entry.args.format;
    info->handle = entry.args.handle;
    info->height = entry.args.height;
    info->width = entry.args.width;
    info->depth = entry.args.depth;
    info->flags = entry.args.flags;
    info->tex_id = 0;
    return 0;
}

void VirtioGpuFrontend::flushResource(uint32_t res_handle) {
    auto taskId = mVirtioGpuTimelines->enqueueTask(VirtioGpuRingGlobal{});
    gfxstream::FrameBuffer::getFB()->postWithCallback(
        res_handle, [this, taskId](std::shared_future<void> waitForGpu) {
            waitForGpu.wait();
            mVirtioGpuTimelines->notifyTaskCompletion(taskId);
        });
}

int VirtioGpuFrontend::createRingBlob(VirtioGpuResource& entry, uint32_t res_handle,
                                      const struct stream_renderer_create_blob* create_blob,
                                      const struct stream_renderer_handle* handle) {
    if (mFeatures.ExternalBlob.enabled) {
        std::string name = "shared-memory-" + std::to_string(res_handle);
        auto shmem = std::make_unique<SharedMemory>(name, create_blob->size);
        int ret = shmem->create(0600);
        if (ret) {
            stream_renderer_error("Failed to create shared memory blob");
            return ret;
        }

        entry.hva = shmem->get();
        entry.ringBlob = std::make_shared<RingBlob>(std::move(shmem));

    } else {
        auto mem = std::make_unique<AlignedMemory>(mPageSize, create_blob->size);
        if (mem->addr == nullptr) {
            stream_renderer_error("Failed to allocate ring blob");
            return -ENOMEM;
        }

        entry.hva = mem->addr;
        entry.ringBlob = std::make_shared<RingBlob>(std::move(mem));
    }

    entry.hvaSize = create_blob->size;
    entry.externalAddr = true;
    entry.caching = STREAM_RENDERER_MAP_CACHE_CACHED;

    return 0;
}

int VirtioGpuFrontend::createBlob(uint32_t ctx_id, uint32_t res_handle,
                                  const struct stream_renderer_create_blob* create_blob,
                                  const struct stream_renderer_handle* handle) {
    stream_renderer_debug("ctx:%u res:%u blob-id:%u blob-size:%u", ctx_id, res_handle,
                          create_blob->blob_id, create_blob->size);

    VirtioGpuResource e;
    struct stream_renderer_resource_create_args args = {0};
    std::optional<BlobDescriptorInfo> descriptorInfoOpt = std::nullopt;
    e.args = args;
    e.hostPipe = 0;

    auto ctxIt = mContexts.find(ctx_id);
    if (ctxIt == mContexts.end()) {
        stream_renderer_error("ctx id %u is not found", ctx_id);
        return -EINVAL;
    }

    auto& ctxEntry = ctxIt->second;

    VirtioGpuResourceType blobType = VirtioGpuResourceType::BLOB;

    auto blobIt = ctxEntry.blobMap.find(create_blob->blob_id);
    if (blobIt != ctxEntry.blobMap.end()) {
        auto& create3d = blobIt->second;
        create3d.handle = res_handle;

        const auto VirtioGpuResourceType = getResourceType(create3d);
        switch (VirtioGpuResourceType) {
            case VirtioGpuResourceType::BLOB:
                return -EINVAL;
            case VirtioGpuResourceType::PIPE:
                // Fallthrough for pipe is intended for blob buffers.
            case VirtioGpuResourceType::BUFFER:
                blobType = VirtioGpuResourceType::BUFFER;
                handleCreateResourceBuffer(&create3d);
                descriptorInfoOpt = gfxstream::FrameBuffer::getFB()->exportBuffer(res_handle);
                break;
            case VirtioGpuResourceType::COLOR_BUFFER:
                blobType = VirtioGpuResourceType::COLOR_BUFFER;
                handleCreateResourceColorBuffer(&create3d);
                descriptorInfoOpt = gfxstream::FrameBuffer::getFB()->exportColorBuffer(res_handle);
                break;
        }

        e.args = create3d;
        ctxEntry.blobMap.erase(create_blob->blob_id);
    }

    if (create_blob->blob_id == 0) {
        int ret = createRingBlob(e, res_handle, create_blob, handle);
        if (ret) {
            return ret;
        }
    } else if (mFeatures.ExternalBlob.enabled) {
        if (create_blob->blob_mem == STREAM_BLOB_MEM_GUEST &&
            (create_blob->blob_flags & STREAM_BLOB_FLAG_CREATE_GUEST_HANDLE)) {
#if defined(__linux__) || defined(__QNX__)
            ManagedDescriptor managedHandle(handle->os_handle);
            ExternalObjectManager::get()->addBlobDescriptorInfo(
                ctx_id, create_blob->blob_id, std::move(managedHandle), handle->handle_type, 0,
                std::nullopt);

            e.caching = STREAM_RENDERER_MAP_CACHE_CACHED;
#else
            return -EINVAL;
#endif
        } else {
            if (!descriptorInfoOpt) {
                descriptorInfoOpt = ExternalObjectManager::get()->removeBlobDescriptorInfo(
                    ctx_id, create_blob->blob_id);
            }

            if (descriptorInfoOpt) {
                e.descriptorInfo =
                    std::make_shared<BlobDescriptorInfo>(std::move(*descriptorInfoOpt));
            } else {
                return -EINVAL;
            }

            e.caching = e.descriptorInfo->caching;
        }
    } else {
        auto entryOpt = ExternalObjectManager::get()->removeMapping(ctx_id, create_blob->blob_id);
        if (entryOpt) {
            e.hva = entryOpt->addr;
            e.caching = entryOpt->caching;
            e.hvaSize = create_blob->size;
        } else {
            return -EINVAL;
        }
    }

    e.blobId = create_blob->blob_id;
    e.blobMem = create_blob->blob_mem;
    e.blobFlags = create_blob->blob_flags;
    e.type = blobType;
    e.iov = nullptr;
    e.numIovs = 0;
    e.linear = 0;
    e.linearSize = 0;

    mResources[res_handle] = e;
    return 0;
}

int VirtioGpuFrontend::resourceMap(uint32_t resourceId, void** hvaOut, uint64_t* sizeOut) {
    if (mFeatures.ExternalBlob.enabled) {
        stream_renderer_error("Failed to map resource: external blob enabled.");
        return -EINVAL;
    }

    auto it = mResources.find(resourceId);
    if (it == mResources.end()) {
        if (hvaOut) *hvaOut = nullptr;
        if (sizeOut) *sizeOut = 0;

        stream_renderer_error("Failed to map resource: unknown resource id %s.", resourceId);
        return -EINVAL;
    }

    const auto& entry = it->second;

    if (hvaOut) *hvaOut = entry.hva;
    if (sizeOut) *sizeOut = entry.hvaSize;

    return 0;
}

int VirtioGpuFrontend::resourceUnmap(uint32_t res_handle) {
    auto it = mResources.find(res_handle);
    if (it == mResources.end()) {
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
    auto it = mResources.find(res_handle);
    if (it == mResources.end()) {
        stream_renderer_error("waitSyncResource could not find resource: %d", res_handle);
        return -EINVAL;
    }
    auto& entry = it->second;
    if (VirtioGpuResourceType::COLOR_BUFFER != entry.type) {
        stream_renderer_error("waitSyncResource is undefined for non-ColorBuffer resource.");
        return -EINVAL;
    }

    return gfxstream::FrameBuffer::getFB()->waitSyncColorBuffer(res_handle);
}

int VirtioGpuFrontend::resourceMapInfo(uint32_t resourceId, uint32_t* map_info) {
    auto it = mResources.find(resourceId);
    if (it == mResources.end()) {
        stream_renderer_error("Failed to get resource map info: unknown resource %d.", resourceId);
        return -EINVAL;
    }

    const auto& entry = it->second;
    *map_info = entry.caching;
    return 0;
}

int VirtioGpuFrontend::exportBlob(uint32_t resourceId, struct stream_renderer_handle* handle) {
    auto it = mResources.find(resourceId);
    if (it == mResources.end()) {
        stream_renderer_error("Failed to export blob: unknown resource %d.", resourceId);
        return -EINVAL;
    }

    auto& entry = it->second;
    if (entry.ringBlob && entry.ringBlob->isExportable()) {
        // Handle ownership transferred to VMM, gfxstream keeps the mapping.
#ifdef _WIN32
        handle->os_handle =
            static_cast<int64_t>(reinterpret_cast<intptr_t>(entry.ringBlob->releaseHandle()));
#else
        handle->os_handle = static_cast<int64_t>(entry.ringBlob->releaseHandle());
#endif
        handle->handle_type = STREAM_MEM_HANDLE_TYPE_SHM;
        return 0;
    }

    if (entry.descriptorInfo) {
        DescriptorType rawDescriptor;
        auto rawDescriptorOpt = entry.descriptorInfo->descriptor.release();
        if (rawDescriptorOpt)
            rawDescriptor = *rawDescriptorOpt;
        else
            return -EINVAL;

        handle->handle_type = entry.descriptorInfo->handleType;

#ifdef _WIN32
        handle->os_handle = static_cast<int64_t>(reinterpret_cast<intptr_t>(rawDescriptor));
#else
        handle->os_handle = static_cast<int64_t>(rawDescriptor);
#endif

        return 0;
    }

    return -EINVAL;
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

int VirtioGpuFrontend::vulkanInfo(uint32_t res_handle,
                                  struct stream_renderer_vulkan_info* vulkan_info) {
    auto it = mResources.find(res_handle);
    if (it == mResources.end()) return -EINVAL;

    const auto& entry = it->second;
    if (entry.descriptorInfo && entry.descriptorInfo->vulkanInfoOpt) {
        vulkan_info->memory_index = (*entry.descriptorInfo->vulkanInfoOpt).memoryIndex;
        memcpy(vulkan_info->device_id.device_uuid,
               (*entry.descriptorInfo->vulkanInfoOpt).deviceUUID,
               sizeof(vulkan_info->device_id.device_uuid));
        memcpy(vulkan_info->device_id.driver_uuid,
               (*entry.descriptorInfo->vulkanInfoOpt).driverUUID,
               sizeof(vulkan_info->device_id.driver_uuid));
        return 0;
    }

    return -EINVAL;
}

#ifdef CONFIG_AEMU
void VirtioGpuFrontend::setServiceOps(const GoldfishPipeServiceOps* ops) { mServiceOps = ops; }
#endif  // CONFIG_AEMU

void VirtioGpuFrontend::allocResource(VirtioGpuResource& entry, iovec* iov, int num_iovs) {
    stream_renderer_debug("entry linear: %p", entry.linear);
    if (entry.linear) free(entry.linear);

    size_t linearSize = 0;
    for (uint32_t i = 0; i < num_iovs; ++i) {
        stream_renderer_debug("iov base: %p", iov[i].iov_base);
        linearSize += iov[i].iov_len;
        stream_renderer_debug("has iov of %zu. linearSize current: %zu", iov[i].iov_len,
                              linearSize);
    }
    stream_renderer_debug("final linearSize: %zu", linearSize);

    void* linear = nullptr;

    if (linearSize) linear = malloc(linearSize);

    entry.numIovs = num_iovs;
    entry.iov = (iovec*)malloc(sizeof(*iov) * num_iovs);
    if (entry.numIovs > 0) {
        memcpy(entry.iov, iov, num_iovs * sizeof(*iov));
    }
    entry.linear = linear;
    entry.linearSize = linearSize;
}

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

    resource.hostPipe = 0;
    resource.ctxId = 0;

    auto ctxIt = mContexts.find(ctxId);
    if (ctxIt != mContexts.end()) {
        auto& ctxEntry = ctxIt->second;
        if (ctxEntry.addressSpaceHandles.count(toUnrefId)) {
            uint32_t asgHandle = ctxEntry.addressSpaceHandles[toUnrefId];

            mCleanupThread->enqueueCleanup([this, asgBlob = resource.ringBlob, asgHandle]() {
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
        auto resourceSnapshotOpt = SnapshotResource(resource);
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
        auto resourceOpt = RestoreResource(resourceSnapshot);
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
