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
#pragma once

#ifndef HOST_BUILD
#include "drm.h"
#endif

struct drm_virtgpu_execbuffer_with_ring_idx {
    uint32_t flags;
    uint32_t size;
    uint64_t command; /* void* */
    uint64_t bo_handles;
    uint32_t num_bo_handles;
    int64_t fence_fd; /* in/out fence fd (see VIRTGPU_EXECBUF_FENCE_FD_IN/OUT) */
    uint32_t ring_idx; /* command ring index (see VIRTGPU_EXECBUF_RING_IDX) */
    uint32_t pad;
};

#ifndef DRM_VIRTGPU_CONTEXT_INIT

#define DRM_VIRTGPU_CONTEXT_INIT 0x0b
#define VIRTGPU_EXECBUF_RING_IDX 0x04

#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID       0x0001
#define VIRTGPU_CONTEXT_PARAM_NUM_RINGS       0x0002
#define VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK 0x0003
#define VIRTGPU_EXECBUF_FENCE_CONTEXT 0x04

struct drm_virtgpu_context_set_param {
    uint64_t param;
    uint64_t value;
};

struct drm_virtgpu_context_init {
    uint32_t num_params;
    uint32_t pad;

    /* pointer to drm_virtgpu_context_set_param array */
    uint64_t ctx_set_params;
};

/*
 *  * Queues an event on a fence to be delivered on the drm character device
 *   * when a fence from a pollable fence context has been signaled.  The param
 *    * VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK specifies pollable rings during
 *     * context creation.
 *      */
#define DRM_VIRTGPU_EVENT_FENCE_SIGNALED 0x80000000

struct drm_virtgpu_event_fence {
    struct drm_event base;
    uint32_t ring_idx;
    uint32_t pad;
};

#else

#endif

#define DRM_IOCTL_VIRTGPU_CONTEXT_INIT                  \
        DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_CONTEXT_INIT,       \
                        struct drm_virtgpu_context_init)
