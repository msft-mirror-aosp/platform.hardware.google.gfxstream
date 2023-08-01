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

VirtGpuBlob::VirtGpuBlob(int64_t deviceHandle, uint32_t blobHandle, uint32_t resourceHandle,
                         uint64_t size)
    : mDeviceHandle(deviceHandle),
      mBlobHandle(blobHandle),
      mResourceHandle(resourceHandle),
      mSize(size) {}

VirtGpuBlob::~VirtGpuBlob(void) {
    struct drm_gem_close gem_close {
        .handle = mBlobHandle, .pad = 0,
    };

    int ret = drmIoctl(mDeviceHandle, DRM_IOCTL_GEM_CLOSE, &gem_close);
    if (ret) {
        ALOGE("DRM_IOCTL_GEM_CLOSE failed with : [%s, blobHandle %u, resourceHandle: %u]",
              strerror(errno), mBlobHandle, mResourceHandle);
    }
}

uint32_t VirtGpuBlob::getBlobHandle(void) {
    return mBlobHandle;
}

uint32_t VirtGpuBlob::getResourceHandle(void) {
    return mResourceHandle;
}

VirtGpuBlobMappingPtr VirtGpuBlob::createMapping(void) {
    int ret;
    struct drm_virtgpu_map map {
        .handle = mBlobHandle, .pad = 0,
    };

    ret = drmIoctl(mDeviceHandle, DRM_IOCTL_VIRTGPU_MAP, &map);
    if (ret) {
        ALOGE("DRM_IOCTL_VIRTGPU_MAP failed with %s", strerror(errno));
        return nullptr;
    }

    uint8_t* ptr = static_cast<uint8_t*>(
            mmap64(nullptr, mSize, PROT_WRITE | PROT_READ, MAP_SHARED, mDeviceHandle, map.offset));

    if (ptr == MAP_FAILED) {
        ALOGE("mmap64 failed with (%s)", strerror(errno));
        return nullptr;
    }

    return std::make_shared<VirtGpuBlobMapping>(shared_from_this(), ptr, mSize);
}

int VirtGpuBlob::exportBlob(struct VirtGpuExternalHandle& handle) {
    int ret, fd;

    ret = drmPrimeHandleToFD(mDeviceHandle, mBlobHandle, DRM_CLOEXEC | DRM_RDWR, &fd);
    if (ret) {
        ALOGE("drmPrimeHandleToFD failed with %s", strerror(errno));
        return ret;
    }

    handle.osHandle = static_cast<int64_t>(fd);
    handle.type = kMemHandleDmabuf;
    return 0;
}

int VirtGpuBlob::wait() {
    int ret;
    struct drm_virtgpu_3d_wait wait_3d = {0};

    int retry = 0;
    do {
        if (retry > 0 && (retry % 10 == 0)) {
            ALOGE("DRM_IOCTL_VIRTGPU_WAIT failed with EBUSY for %d times.", retry);
        }
        wait_3d.handle = mBlobHandle;
        ret = drmIoctl(mDeviceHandle, DRM_IOCTL_VIRTGPU_WAIT, &wait_3d);
        ++retry;
    } while (ret < 0 && errno == EBUSY);

    if (ret < 0) {
        ALOGE("DRM_IOCTL_VIRTGPU_WAIT failed with %s", strerror(errno));
        return ret;
    }

    return 0;
}
