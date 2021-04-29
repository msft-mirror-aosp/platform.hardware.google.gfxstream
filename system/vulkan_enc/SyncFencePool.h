// Copyright (C) 2021 The Android Open Source Project
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

#ifdef VK_USE_PLATFORM_FUCHSIA
#include <fuchsia/hardware/goldfish/llcpp/fidl.h>
#endif  // VK_USE_PLATFORM_FUCHSIA

#include "ResourceTracker.h"
#include "VkEncoder.h"

#include "android/base/synchronization/AndroidObjectPool.h"

#include <log/log.h>
#include <vulkan/vulkan.h>

namespace goldfish_vk {

#ifdef VK_USE_PLATFORM_FUCHSIA
using SyncDeviceClient = fidl::WireSyncClient<fuchsia_hardware_goldfish::SyncDevice>;
using SyncTimelineClient = fidl::WireSyncClient<fuchsia_hardware_goldfish::SyncTimeline>;

#else  // !VK_USE_PLATFORM_FUCHSIA
using SyncDeviceClient = int;
using SyncTimelineClient = int;

#endif  // VK_USE_PLATFORM_FUCHSIA

// A SyncFence is a pair of a normal VkFence dedicated for host / guest sync
// purposes and a goldfish sync timeline associated with that fence.
//
// When guest calls vkQueueSubmit() and it needs to wait for the submitted
// commands to finish, it acquires a SyncFence, calls vkQueueSubmit() with the
// |fence| variable from |SyncFence|, and triggers a VkFence wait using the
// |timelineClient| member variable.
struct SyncFence {
    VkFence fence;
#ifdef VK_USE_PLATFORM_FUCHSIA
    SyncTimelineClient timelineClient;
#endif  // VK_USE_PLATFORM_FUCHSIA
};

// A SyncFencePool stores multiple SyncFence objects and allows for reuse.
// Every time when clients need a SyncFence, it calls |acquire()| from
// SyncFencePool to get an fence, and returns it by calling |release()| after
// it finishes using the fence.
class SyncFencePool : public android::base::guest::ObjectPool<SyncFence> {
public:
    SyncFencePool(VkDevice device, SyncDeviceClient* syncDevice);
    ~SyncFencePool() = default;

private:
    // unlimited
    constexpr static size_t kPoolSizeLimit = 0u;
};

}  // namespace goldfish_vk
