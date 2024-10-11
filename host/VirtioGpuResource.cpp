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
    resourceSnapshot.set_id(resource.args.handle);

    VirtioGpuResourceCreateArgs* snapshotCreateArgs = resourceSnapshot.mutable_create_args();
    snapshotCreateArgs->set_id(resource.args.handle);
    snapshotCreateArgs->set_target(resource.args.target);
    snapshotCreateArgs->set_format(resource.args.format);
    snapshotCreateArgs->set_bind(resource.args.bind);
    snapshotCreateArgs->set_width(resource.args.width);
    snapshotCreateArgs->set_height(resource.args.height);
    snapshotCreateArgs->set_depth(resource.args.depth);
    snapshotCreateArgs->set_array_size(resource.args.array_size);
    snapshotCreateArgs->set_last_level(resource.args.last_level);
    snapshotCreateArgs->set_nr_samples(resource.args.nr_samples);
    snapshotCreateArgs->set_flags(resource.args.flags);

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
            stream_renderer_error("Failed to snapshot ring blob for resource %d",
                                  resource.args.handle);
            return std::nullopt;
        }
        resourceSnapshot.mutable_ring_blob()->Swap(&*snapshotRingBlobOpt);
    }

    return resourceSnapshot;
}

std::optional<VirtioGpuResource> RestoreResource(
        const VirtioGpuResourceSnapshot& resourceSnapshot) {
    VirtioGpuResource resource = {};

    const auto& resourceCreateArgsSnapshot = resourceSnapshot.create_args();
    resource.args.handle = resourceCreateArgsSnapshot.id();
    resource.args.target = resourceCreateArgsSnapshot.target();
    resource.args.format = resourceCreateArgsSnapshot.format();
    resource.args.bind = resourceCreateArgsSnapshot.bind();
    resource.args.width = resourceCreateArgsSnapshot.width();
    resource.args.height = resourceCreateArgsSnapshot.height();
    resource.args.depth = resourceCreateArgsSnapshot.depth();
    resource.args.array_size = resourceCreateArgsSnapshot.array_size();
    resource.args.last_level = resourceCreateArgsSnapshot.last_level();
    resource.args.nr_samples = resourceCreateArgsSnapshot.nr_samples();
    resource.args.flags = resourceCreateArgsSnapshot.flags();

    if (resourceSnapshot.has_create_blob_args()) {
        const auto& snapshotCreateArgs = resourceSnapshot.create_blob_args();
        resource.createBlobArgs = {
            .blob_mem = snapshotCreateArgs.mem(),
            .blob_flags = snapshotCreateArgs.flags(),
            .blob_id = snapshotCreateArgs.id(),
            .size = snapshotCreateArgs.size(),
        };
    }

    if (resourceSnapshot.has_ring_blob()) {
        auto resourceRingBlobOpt = RingBlob::Restore(resourceSnapshot.ring_blob());
        if (!resourceRingBlobOpt) {
            stream_renderer_error("Failed to restore ring blob for resource %d",
                                  resource.args.handle);
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