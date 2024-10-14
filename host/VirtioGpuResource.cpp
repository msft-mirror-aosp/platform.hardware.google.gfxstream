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

#include "VirtioGpuResource.h"

namespace gfxstream {
namespace host {

#ifdef GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT

using gfxstream::host::snapshot::VirtioGpuResourceCreateArgs;
using gfxstream::host::snapshot::VirtioGpuResourceCreateBlobArgs;
using gfxstream::host::snapshot::VirtioGpuResourceSnapshot;

std::optional<VirtioGpuResourceSnapshot> SnapshotResource(const VirtioGpuResource& resource) {
    VirtioGpuResourceSnapshot resourceSnapshot;
    resourceSnapshot.set_id(resource.id);

    if (resource.createArgs) {
        VirtioGpuResourceCreateArgs* snapshotCreateArgs = resourceSnapshot.mutable_create_args();
        snapshotCreateArgs->set_id(resource.createArgs->handle);
        snapshotCreateArgs->set_target(resource.createArgs->target);
        snapshotCreateArgs->set_format(resource.createArgs->format);
        snapshotCreateArgs->set_bind(resource.createArgs->bind);
        snapshotCreateArgs->set_width(resource.createArgs->width);
        snapshotCreateArgs->set_height(resource.createArgs->height);
        snapshotCreateArgs->set_depth(resource.createArgs->depth);
        snapshotCreateArgs->set_array_size(resource.createArgs->array_size);
        snapshotCreateArgs->set_last_level(resource.createArgs->last_level);
        snapshotCreateArgs->set_nr_samples(resource.createArgs->nr_samples);
        snapshotCreateArgs->set_flags(resource.createArgs->flags);
    }

    if (resource.createBlobArgs) {
        VirtioGpuResourceCreateBlobArgs* snapshotCreateArgs = resourceSnapshot.mutable_create_blob_args();
        snapshotCreateArgs->set_mem(resource.createBlobArgs->blob_mem);
        snapshotCreateArgs->set_flags(resource.createBlobArgs->blob_flags);
        snapshotCreateArgs->set_id(resource.createBlobArgs->blob_id);
        snapshotCreateArgs->set_size(resource.createBlobArgs->size);
    }

    if (resource.ringBlob) {
        auto snapshotRingBlobOpt = resource.ringBlob->Snapshot();
        if (!snapshotRingBlobOpt) {
            stream_renderer_error("Failed to snapshot ring blob for resource %d.", resource.id);
            return std::nullopt;
        }
        resourceSnapshot.mutable_ring_blob()->Swap(&*snapshotRingBlobOpt);
    }

    return resourceSnapshot;
}

std::optional<VirtioGpuResource> RestoreResource(
        const VirtioGpuResourceSnapshot& resourceSnapshot) {
    VirtioGpuResource resource = {};

    if (resourceSnapshot.has_create_args()) {
        const auto& createArgsSnapshot = resourceSnapshot.create_args();
        resource.createArgs = {
            .handle = createArgsSnapshot.id(),
            .target = createArgsSnapshot.target(),
            .format = createArgsSnapshot.format(),
            .bind = createArgsSnapshot.bind(),
            .width = createArgsSnapshot.width(),
            .height = createArgsSnapshot.height(),
            .depth = createArgsSnapshot.depth(),
            .array_size = createArgsSnapshot.array_size(),
            .last_level = createArgsSnapshot.last_level(),
            .nr_samples = createArgsSnapshot.nr_samples(),
            .flags = createArgsSnapshot.flags(),
        };
    }

    if (resourceSnapshot.has_create_blob_args()) {
        const auto& createArgsSnapshot = resourceSnapshot.create_blob_args();
        resource.createBlobArgs = {
            .blob_mem = createArgsSnapshot.mem(),
            .blob_flags = createArgsSnapshot.flags(),
            .blob_id = createArgsSnapshot.id(),
            .size = createArgsSnapshot.size(),
        };
    }

    if (resourceSnapshot.has_ring_blob()) {
        auto resourceRingBlobOpt = RingBlob::Restore(resourceSnapshot.ring_blob());
        if (!resourceRingBlobOpt) {
            stream_renderer_error("Failed to restore ring blob for resource %d", resource.id);
            return std::nullopt;
        }
        resource.ringBlob = std::move(*resourceRingBlobOpt);
        resource.hva = resource.ringBlob->map();
        resource.hvaSize = resource.ringBlob->size();
    }

    return resource;
}

#endif

}  // namespace host
}  // namespace gfxstream