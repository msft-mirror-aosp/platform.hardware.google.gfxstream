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

#include "VirtioGpuContext.h"

namespace gfxstream {
namespace host {

#ifdef GFXSTREAM_BUILD_WITH_SNAPSHOT_FRONTEND_SUPPORT

using gfxstream::host::snapshot::VirtioGpuContextSnapshot;

std::optional<VirtioGpuContextSnapshot> SnapshotContext(const VirtioGpuContext& context) {
    VirtioGpuContextSnapshot contextSnapshot;
    contextSnapshot.set_id(context.ctxId);
    contextSnapshot.set_name(context.name);
    contextSnapshot.set_capset(context.capsetId);
    contextSnapshot.mutable_resource_asgs()->insert(context.addressSpaceHandles.begin(),
                                                    context.addressSpaceHandles.end());
    return contextSnapshot;
}

std::optional<VirtioGpuContext> RestoreContext(
        const VirtioGpuContextSnapshot& contextSnapshot) {
    VirtioGpuContext context = {};
    context.ctxId = contextSnapshot.id();
    context.name = contextSnapshot.name();
    context.capsetId = contextSnapshot.capset();
    context.addressSpaceHandles.insert(contextSnapshot.resource_asgs().begin(),
                                       contextSnapshot.resource_asgs().end());
    return context;
}

#endif

}  // namespace host
}  // namespace gfxstream