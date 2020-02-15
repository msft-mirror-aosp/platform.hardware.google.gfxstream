// Copyright (C) 2020 The Android Open Source Project
// Copyright (C) 2020 Google Inc.
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

#define DRM_VIRTGPU_RESOURCE_CREATE_V2 0x0a

struct drm_virtgpu_resource_create_v2 {
#define VIRTGPU_RESOURCE_TYPE_MASK       0x0000f
#define VIRTGPU_RESOURCE_TYPE_DEFAULT_V1 0x00001
#define VIRTGPU_RESOURCE_TYPE_DEFAULT_V2 0x00002
#define VIRTGPU_RESOURCE_TYPE_HOST       0x00003
#define VIRTGPU_RESOURCE_TYPE_GUEST      0x00004
/*
 * Error cases:
 * HOST_VISIBLE_BIT without VIRTGPU_RESOURCE_TYPE_HOST
 */
#define VIRTGPU_RESOURCE_HOST_MASK            0x000f0
#define VIRTGPU_RESOURCE_HOST_VISIBLE_BIT     0x00010

#define VIRTGPU_RESOURCE_GUEST_MASK                  0x00f00
#define VIRTGPU_RESOURCE_GUEST_SHARED_BIT            0x00100
#define VIRTGPU_RESOURCE_GUEST_EMULATED_COHERENT_BIT 0x00200

#define VIRTGPU_RESOURCE_CACHE_MASK      0x0f000
#define VIRTGPU_RESOURCE_CACHE_CACHED    0x01000
#define VIRTGPU_RESOURCE_CACHE_UNCACHED  0x02000
#define VIRTGPU_RESOURCE_CACHE_WC        0x03000
/*
 * VIRTGPU_RESOURCE_EXPORTABLE_BIT - host resource *can* be exported as an fd.
 */
#define VIRTGPU_RESOURCE_EXPORT_MASK    0xf0000
#define VIRTGPU_RESOURCE_EXPORTABLE_BIT 0x10000
	uint32_t flags;
	uint32_t args_size;
	uint64_t size;
	uint32_t bo_handle;
	uint32_t res_handle;
	uint64_t args;
};

#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_V2				\
	DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE_V2,	\
		struct drm_virtgpu_resource_create_v2)

