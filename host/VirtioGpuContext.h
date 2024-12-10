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
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "ExternalObjectManager.h"
#include "VirtioGpu.h"
#ifdef GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT
// X11 defines status as a preprocessor define which messes up
// anyone with a `Status` type.
#undef Status
#include "VirtioGpuContextSnapshot.pb.h"
#endif
#include "VirtioGpuResource.h"
#include "gfxstream/virtio-gpu-gfxstream-renderer.h"
#include "host-common/address_space_device_control_ops.h"
extern "C" {
#include "host-common/goldfish_pipe.h"
}  // extern "C"

namespace gfxstream {
namespace host {

class VirtioGpuContext {
   public:
    VirtioGpuContext() = default;

    static std::optional<VirtioGpuContext> Create(const GoldfishPipeServiceOps* ops,
                                                  VirtioGpuContextId contextId,
                                                  const std::string& contextName,
                                                  uint32_t capsetId);

    int Destroy(const GoldfishPipeServiceOps* pipeOps,
                const struct address_space_device_control_ops* asgOps);

    void AttachResource(VirtioGpuResource& resource);
    void DetachResource(VirtioGpuResource& resource);

    const std::unordered_set<VirtioGpuResourceId>& GetAttachedResources() const;

    void SetHostPipe(GoldfishHostPipe* pipe);

    int AcquireSync(uint64_t syncId);
    std::optional<SyncDescriptorInfo> TakeSync();

    int CreateAddressSpaceGraphicsInstance(const struct address_space_device_control_ops* asgOps,
                                           VirtioGpuResource& resource);
    const std::unordered_map<VirtioGpuResourceId, uint32_t>& AsgInstances() const;
    std::optional<uint32_t> TakeAddressSpaceGraphicsHandle(VirtioGpuResourceId resourceId);

    int PingAddressSpaceGraphicsInstance(const struct address_space_device_control_ops* asgOps,
                                         VirtioGpuResourceId resourceId);

    int AddPendingBlob(uint32_t blobId, struct stream_renderer_resource_create_args args);
    std::optional<struct stream_renderer_resource_create_args> TakePendingBlob(uint32_t args);

#ifdef GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT
    std::optional<gfxstream::host::snapshot::VirtioGpuContextSnapshot> Snapshot() const;

    static std::optional<VirtioGpuContext> Restore(
        const gfxstream::host::snapshot::VirtioGpuContextSnapshot& snapshot);
#endif

   private:
    // LINT.IfChange(virtio_gpu_context)
    VirtioGpuContextId mId;
    std::string mName;
    uint32_t mCapsetId;
    GoldfishHostPipe* mHostPipe;
    std::unordered_set<VirtioGpuResourceId> mAttachedResources;
    std::unordered_map<VirtioGpuResourceId, uint32_t> mAddressSpaceHandles;
    std::unordered_map<uint32_t, struct stream_renderer_resource_create_args> mPendingBlobs;
    std::optional<SyncDescriptorInfo> mLatestSync;
    // LINT.ThenChange(VirtioGpuContextSnapshot.proto:virtio_gpu_context)
};

}  // namespace host
}  // namespace gfxstream