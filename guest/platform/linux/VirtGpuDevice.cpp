/*
 * Copyright 2022 The Android Open Source Project
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

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <cerrno>
#include <cstring>

#include <cutils/log.h>

#include "VirtGpu.h"
#include "virtgpu_drm.h"
#include "virtgpu_gfxstream_protocol.h"

#define PARAM(x) \
    (struct VirtGpuParam) { x, #x, 0 }

// See virgl_hw.h and p_defines.h
#define VIRGL_FORMAT_R8_UNORM 64
#define VIRGL_BIND_CUSTOM (1 << 17)
#define PIPE_BUFFER 0

VirtGpuDevice& VirtGpuDevice::getInstance(enum VirtGpuCapset capset) {
    static VirtGpuDevice mInstance(capset);
    return mInstance;
}

VirtGpuDevice::VirtGpuDevice(enum VirtGpuCapset capset) {
    struct VirtGpuParam params[] = {
        PARAM(VIRTGPU_PARAM_3D_FEATURES),          PARAM(VIRTGPU_PARAM_CAPSET_QUERY_FIX),
        PARAM(VIRTGPU_PARAM_RESOURCE_BLOB),        PARAM(VIRTGPU_PARAM_HOST_VISIBLE),
        PARAM(VIRTGPU_PARAM_CROSS_DEVICE),         PARAM(VIRTGPU_PARAM_CONTEXT_INIT),
        PARAM(VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs), PARAM(VIRTGPU_PARAM_CREATE_GUEST_HANDLE),
    };

    int ret;
    struct drm_virtgpu_get_caps get_caps = {0};
    struct drm_virtgpu_context_init init = {0};
    struct drm_virtgpu_context_set_param ctx_set_params[2] = {{0}};

    memset(&mCaps, 0, sizeof(struct VirtGpuCaps));

    mDeviceHandle = static_cast<int64_t>(drmOpenRender(128));
    if (mDeviceHandle < 0) {
        ALOGE("Failed to open rendernode: %s", strerror(errno));
        return;
    }

    for (uint32_t i = 0; i < kParamMax; i++) {
        struct drm_virtgpu_getparam get_param = {0};
        get_param.param = params[i].param;
        get_param.value = (uint64_t)(uintptr_t)&params[i].value;

        ret = drmIoctl(mDeviceHandle, DRM_IOCTL_VIRTGPU_GETPARAM, &get_param);
        if (ret) {
            ALOGE("virtgpu backend not enabling %s", params[i].name);
            continue;
        }

        mCaps.params[i] = params[i].value;
    }

    get_caps.cap_set_id = static_cast<uint32_t>(capset);
    if (capset == kCapsetGfxStreamVulkan) {
        get_caps.size = sizeof(struct gfxstreamCapset);
        get_caps.addr = (unsigned long long)&mCaps.gfxstreamCapset;
    }

    ret = drmIoctl(mDeviceHandle, DRM_IOCTL_VIRTGPU_GET_CAPS, &get_caps);
    if (ret) {
        // Don't fail get capabilities just yet, AEMU doesn't use this API
        // yet (b/272121235);
        ALOGE("DRM_IOCTL_VIRTGPU_GET_CAPS failed with %s", strerror(errno));
    }

    // We always need an ASG blob in some cases, so always define blobAlignment
    if (!mCaps.gfxstreamCapset.blobAlignment) {
        mCaps.gfxstreamCapset.blobAlignment = 4096;
    }

    ctx_set_params[0].param = VIRTGPU_CONTEXT_PARAM_NUM_RINGS;
    ctx_set_params[0].value = 2;
    init.num_params = 1;

    if (capset != kCapsetNone) {
        ctx_set_params[1].param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID;
        ctx_set_params[1].value = static_cast<uint32_t>(capset);
        init.num_params++;
    }

    init.ctx_set_params = (unsigned long long)&ctx_set_params[0];
    ret = drmIoctl(mDeviceHandle, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &init);
    if (ret) {
        ALOGE("DRM_IOCTL_VIRTGPU_CONTEXT_INIT failed with %s, continuing without context...",
               strerror(errno));
    }
}

struct VirtGpuCaps VirtGpuDevice::getCaps(void) { return mCaps; }

int64_t VirtGpuDevice::getDeviceHandle(void) {
    return mDeviceHandle;
}

VirtGpuBlobPtr VirtGpuDevice::createPipeBlob(uint32_t size) {
    drm_virtgpu_resource_create create = {
            .target = PIPE_BUFFER,
            .format = VIRGL_FORMAT_R8_UNORM,
            .bind = VIRGL_BIND_CUSTOM,
            .width = size,
            .height = 1U,
            .depth = 1U,
            .array_size = 0U,
            .size = size,
            .stride = size,
    };

    int ret = drmIoctl(mDeviceHandle, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &create);
    if (ret) {
        ALOGE("DRM_IOCTL_VIRTGPU_RESOURCE_CREATE failed with %s", strerror(errno));
        return nullptr;
    }

    return std::make_shared<VirtGpuBlob>(mDeviceHandle, create.bo_handle, create.res_handle,
                                         static_cast<uint64_t>(size));
}

VirtGpuBlobPtr VirtGpuDevice::createBlob(const struct VirtGpuCreateBlob& blobCreate) {
    int ret;
    struct drm_virtgpu_resource_create_blob create = {0};

    create.size = blobCreate.size;
    create.blob_mem = blobCreate.blobMem;
    create.blob_flags = blobCreate.flags;
    create.blob_id = blobCreate.blobId;

    ret = drmIoctl(mDeviceHandle, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &create);
    if (ret < 0) {
        ALOGE("DRM_VIRTGPU_RESOURCE_CREATE_BLOB failed with %s", strerror(errno));
        return nullptr;
    }

    return std::make_shared<VirtGpuBlob>(mDeviceHandle, create.bo_handle, create.res_handle,
                                         blobCreate.size);
}

VirtGpuBlobPtr VirtGpuDevice::importBlob(const struct VirtGpuExternalHandle& handle) {
    struct drm_virtgpu_resource_info info = {0};
    uint32_t blobHandle;
    int ret;

    ret = drmPrimeFDToHandle(mDeviceHandle, handle.osHandle, &blobHandle);
    close(handle.osHandle);
    if (ret) {
        ALOGE("DRM_IOCTL_PRIME_FD_TO_HANDLE failed: %s", strerror(errno));
        return nullptr;
    }

    info.bo_handle = blobHandle;
    ret = drmIoctl(mDeviceHandle, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info);
    if (ret) {
        ALOGE("DRM_IOCTL_VIRTGPU_RESOURCE_INFO failed: %s", strerror(errno));
        return nullptr;
    }

    return std::make_shared<VirtGpuBlob>(mDeviceHandle, blobHandle, info.res_handle,
                                         static_cast<uint64_t>(info.size));
}

int VirtGpuDevice::execBuffer(struct VirtGpuExecBuffer& execbuffer, VirtGpuBlobPtr blob) {
    int ret;
    struct drm_virtgpu_execbuffer exec = {0};
    uint32_t blobHandle;

    exec.flags = execbuffer.flags;
    exec.size = execbuffer.command_size;
    exec.ring_idx = execbuffer.ring_idx;
    exec.command = (uint64_t)(uintptr_t)(execbuffer.command);
    exec.fence_fd = -1;

    if (blob) {
        blobHandle = blob->getBlobHandle();
        exec.bo_handles = (uint64_t)(uintptr_t)(&blobHandle);
        exec.num_bo_handles = 1;
    }

    ret = drmIoctl(mDeviceHandle, DRM_IOCTL_VIRTGPU_EXECBUFFER, &exec);
    if (ret) {
        ALOGE("DRM_IOCTL_VIRTGPU_EXECBUFFER failed: %s", strerror(errno));
        return ret;
    }

    if (execbuffer.flags & kFenceOut) {
        execbuffer.handle.osHandle = exec.fence_fd;
        execbuffer.handle.type = kFenceHandleSyncFd;
    }

    return 0;
}

VirtGpuDevice::~VirtGpuDevice() {
    close(mDeviceHandle);
}
