// Copyright 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either expresso or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "GrallocMinigbm.h"

#if defined(VIRTIO_GPU)
#include <cros_gralloc/cros_gralloc_handle.h>
#include <errno.h>
#include <xf86drm.h>
#include <unistd.h>

#include <cinttypes>
#include <cstring>

#include "virtgpu_drm.h"
static const size_t kPageSize = getpagesize();
#else
constexpr size_t kPageSize = PAGE_SIZE;
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

namespace gfxstream {

#if defined(VIRTIO_GPU)

namespace {

static inline uint32_t align_up(uint32_t n, uint32_t a) { return ((n + a - 1) / a) * a; }

bool getVirtioGpuResourceInfo(int fd, native_handle_t const* handle,
                              struct drm_virtgpu_resource_info* info) {
    memset(info, 0x0, sizeof(*info));
    if (fd < 0) {
        ALOGE("%s: Error, rendernode fd missing\n", __func__);
        return false;
    }

    struct drm_gem_close gem_close;
    memset(&gem_close, 0x0, sizeof(gem_close));

    cros_gralloc_handle const* cros_handle = reinterpret_cast<cros_gralloc_handle const*>(handle);

    uint32_t prime_handle;
    int ret = drmPrimeFDToHandle(fd, cros_handle->fds[0], &prime_handle);
    if (ret) {
        ALOGE("%s: DRM_IOCTL_PRIME_FD_TO_HANDLE failed: %s (errno %d)\n", __func__, strerror(errno),
              errno);
        return false;
    }
    struct ManagedDrmGem {
        ManagedDrmGem(const ManagedDrmGem&) = delete;
        ~ManagedDrmGem() {
            struct drm_gem_close gem_close {
                .handle = m_prime_handle, .pad = 0,
            };
            int ret = drmIoctl(m_fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
            if (ret) {
                ALOGE("%s: DRM_IOCTL_GEM_CLOSE failed on handle %" PRIu32 ": %s(%d).", __func__,
                      m_prime_handle, strerror(errno), errno);
            }
        }

        int m_fd;
        uint32_t m_prime_handle;
    } managed_prime_handle{
        .m_fd = fd,
        .m_prime_handle = prime_handle,
    };

    info->bo_handle = managed_prime_handle.m_prime_handle;

    struct drm_virtgpu_3d_wait virtgpuWait {
        .handle = managed_prime_handle.m_prime_handle, .flags = 0,
    };
    // This only works for host resources by VIRTGPU_RESOURCE_CREATE ioctl.
    // We need to use a different mechanism to synchronize with the host if
    // the minigbm gralloc swiches to virtio-gpu blobs or cross-domain
    // backend.
    ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_WAIT, &virtgpuWait);
    if (ret) {
        ALOGE("%s: DRM_IOCTL_VIRTGPU_WAIT failed: %s(%d)", __func__, strerror(errno), errno);
        return false;
    }

    ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, info);
    if (ret) {
        ALOGE("%s: DRM_IOCTL_VIRTGPU_RESOURCE_INFO failed: %s (errno %d)\n", __func__,
              strerror(errno), errno);
        return false;
    }

    return true;
}

}  // namespace

uint32_t MinigbmGralloc::createColorBuffer(renderControl_client_context_t*, int width, int height,
                                           uint32_t glformat) {
    // Only supported format for pbuffers in gfxstream should be RGBA8
    const uint32_t kGlRGB = 0x1907;
    const uint32_t kGlRGBA = 0x1908;
    const uint32_t kVirglFormatRGBA = 67;  // VIRGL_FORMAT_R8G8B8A8_UNORM;
    uint32_t virtgpu_format = 0;
    uint32_t bpp = 0;
    switch (glformat) {
        case kGlRGB:
            ALOGV("Note: egl wanted GL_RGB, still using RGBA");
            virtgpu_format = kVirglFormatRGBA;
            bpp = 4;
            break;
        case kGlRGBA:
            virtgpu_format = kVirglFormatRGBA;
            bpp = 4;
            break;
        default:
            ALOGV("Note: egl wanted 0x%x, still using RGBA", glformat);
            virtgpu_format = kVirglFormatRGBA;
            bpp = 4;
            break;
    }
    const uint32_t kPipeTexture2D = 2;          // PIPE_TEXTURE_2D
    const uint32_t kBindRenderTarget = 1 << 1;  // VIRGL_BIND_RENDER_TARGET
    struct drm_virtgpu_resource_create res_create;
    memset(&res_create, 0, sizeof(res_create));
    res_create.target = kPipeTexture2D;
    res_create.format = virtgpu_format;
    res_create.bind = kBindRenderTarget;
    res_create.width = width;
    res_create.height = height;
    res_create.depth = 1;
    res_create.array_size = 1;
    res_create.last_level = 0;
    res_create.nr_samples = 0;
    res_create.stride = bpp * width;
    res_create.size = align_up(bpp * width * height, PAGE_SIZE);

    int ret = drmIoctl(m_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &res_create);
    if (ret) {
        ALOGE("%s: DRM_IOCTL_VIRTGPU_RESOURCE_CREATE failed with %s (%d)\n", __func__,
              strerror(errno), errno);
        abort();
    }

    return res_create.res_handle;
}

uint32_t MinigbmGralloc::getHostHandle(native_handle_t const* handle) {
    struct drm_virtgpu_resource_info info;
    if (!getVirtioGpuResourceInfo(m_fd, handle, &info)) {
        ALOGE("%s: failed to get resource info\n", __func__);
        return 0;
    }

    return info.res_handle;
}

int MinigbmGralloc::getFormat(native_handle_t const* handle) {
    return ((cros_gralloc_handle*)handle)->droid_format;
}

uint32_t MinigbmGralloc::getFormatDrmFourcc(native_handle_t const* handle) {
    return ((cros_gralloc_handle*)handle)->format;
}

size_t MinigbmGralloc::getAllocatedSize(native_handle_t const* handle) {
    struct drm_virtgpu_resource_info info;
    if (!getVirtioGpuResourceInfo(m_fd, handle, &info)) {
        ALOGE("%s: failed to get resource info\n", __func__);
        return 0;
    }

    return info.size;
}

#else

uint32_t MinigbmGralloc::createColorBuffer(renderControl_client_context_t*, int width, int height,
                                           uint32_t glformat) {
    ALOGE("%s: Error: using minigbm without -DVIRTIO_GPU\n", __func__);
    return 0;
}

uint32_t MinigbmGralloc::getHostHandle(native_handle_t const* handle) {
    ALOGE("%s: Error: using minigbm without -DVIRTIO_GPU\n", __func__);
    return 0;
}

int MinigbmGralloc::getFormat(native_handle_t const* handle) {
    ALOGE("%s: Error: using minigbm without -DVIRTIO_GPU\n", __func__);
    return 0;
}

uint32_t MinigbmGralloc::getFormatDrmFourcc(native_handle_t const* handle) {
    ALOGE("%s: Error: using minigbm without -DVIRTIO_GPU\n", __func__);
    return 0;
}

size_t MinigbmGralloc::getAllocatedSize(native_handle_t const* handle) {
    ALOGE("%s: Error: using minigbm without -DVIRTIO_GPU\n", __func__);
    return 0;
}

#endif

}  // namespace gfxstream
