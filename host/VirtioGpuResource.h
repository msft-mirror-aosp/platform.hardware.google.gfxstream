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

#pragma once

#include <stdint.h>

#include <memory>
#include <optional>

extern "C" {
#include "host-common/goldfish_pipe.h"
}  // extern "C"

#include "ExternalObjectManager.h"
#include "VirtioGpu.h"
#ifdef GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT
#include "VirtioGpuResourceSnapshot.pb.h"
#endif  // GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT
#include "VirtioGpuRingBlobMemory.h"
#include "gfxstream/virtio-gpu-gfxstream-renderer.h"

namespace gfxstream {
namespace host {

enum class VirtioGpuResourceType {
    // Used as a communication channel between the guest and the host
    // which does not need an allocation on the host GPU.
    PIPE,
    // Used as a GPU data buffer.
    BUFFER,
    // Used as a GPU texture.
    COLOR_BUFFER,
    // Used as a blob and not known to FrameBuffer.
    BLOB,
};

// LINT.IfChange(virtio_gpu_resource)
struct VirtioGpuResource {
    stream_renderer_resource_create_args args;
    std::vector<struct iovec> iovs;
    void* linear;
    size_t linearSize;
    GoldfishHostPipe* hostPipe;
    VirtioGpuContextId ctxId;
    void* hva;
    uint64_t hvaSize;
    uint64_t blobId;
    uint32_t blobMem;
    uint32_t blobFlags;
    uint32_t caching;
    VirtioGpuResourceType type;
    std::shared_ptr<RingBlob> ringBlob;
    bool externalAddr = false;
    std::shared_ptr<BlobDescriptorInfo> descriptorInfo = nullptr;
};
// LINT.ThenChange(VirtioGpuResourceSnapshot.proto:virtio_gpu_resource)

#ifdef GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT
std::optional<gfxstream::host::snapshot::VirtioGpuResourceSnapshot>
SnapshotResource(const VirtioGpuResource& resource);

std::optional<VirtioGpuResource>
RestoreResource(const gfxstream::host::snapshot::VirtioGpuResourceSnapshot& snapshot);
#endif


}  // namespace host
}  // namespace gfxstream