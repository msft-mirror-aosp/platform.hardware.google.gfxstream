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
#include <string>

extern "C" {
#include "host-common/goldfish_pipe.h"
}  // extern "C"

#include "ExternalObjectManager.h"
#include "VirtioGpu.h"
#include "gfxstream/virtio-gpu-gfxstream-renderer.h"

namespace gfxstream {
namespace host {

struct VirtioGpuContext {
    std::string name;
    uint32_t capsetId;
    VirtioGpuContextId ctxId;
    GoldfishHostPipe* hostPipe;
    int fence;
    uint32_t addressSpaceHandle;
    bool hasAddressSpaceHandle;
    std::unordered_map<VirtioGpuResourceId, uint32_t> addressSpaceHandles;
    std::unordered_map<uint32_t, struct stream_renderer_resource_create_args> blobMap;
    std::shared_ptr<gfxstream::SyncDescriptorInfo> latestFence;
};

}  // namespace host
}  // namespace gfxstream