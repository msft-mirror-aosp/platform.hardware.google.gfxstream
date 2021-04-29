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

#include "SyncFencePool.h"
#include "VkEncoder.h"

#include <log/log.h>
#include <vulkan/vulkan.h>

namespace goldfish_vk {

namespace {

static VkFence newFence(VkDevice device) {
    auto hostConn = ResourceTracker::threadingCallbacks.hostConnectionGetFunc();
    auto vkEncoder = ResourceTracker::threadingCallbacks.vkEncoderGetFunc(hostConn);
    VkFenceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0u,
    };

    VkFence fence = VK_NULL_HANDLE;
    VkResult result = vkEncoder->vkCreateFence(device, &createInfo, 0u, &fence, true /* do lock */);
    if (result != VK_SUCCESS) {
        ALOGE("%s: vkCreateFence failed: result: %d", result);
        return VK_NULL_HANDLE;
    }
    return fence;
}

static void destroyFence(VkDevice device, VkFence fence) {
    auto hostConn = ResourceTracker::threadingCallbacks.hostConnectionGetFunc();
    auto vkEncoder = ResourceTracker::threadingCallbacks.vkEncoderGetFunc(hostConn);

    vkEncoder->vkDestroyFence(device, fence, nullptr, true /* do lock */);
}

static void resetFence(VkDevice device, VkFence fence) {
    auto hostConn = ResourceTracker::threadingCallbacks.hostConnectionGetFunc();
    auto vkEncoder = ResourceTracker::threadingCallbacks.vkEncoderGetFunc(hostConn);

    vkEncoder->vkResetFences(device, 1, &fence, true /* do lock */);
}

static SyncTimelineClient newTimelineClient(SyncDeviceClient* syncDevice) {
#ifdef VK_USE_PLATFORM_FUCHSIA
    auto timelineEnds = fidl::CreateEndpoints<fuchsia_hardware_goldfish::SyncTimeline>();
    if (!timelineEnds.is_ok()) {
        ALOGE("Cannot create sync timeline channels, error: %s", timelineEnds.status_string());
        return {};
    }

    auto createTimelineResult = syncDevice->CreateTimeline(std::move(timelineEnds->server));
    if (!createTimelineResult.ok()) {
        ALOGE("CreateTimeline failed, error: %s", createTimelineResult.status_string());
        return {};
    }

    return fidl::BindSyncClient(std::move(timelineEnds->client));
#else   // !VK_USE_PLATFORM_FUCHSIA
    (void)syncDevice;
    return 0;
#endif  // VK_USE_PLATFORM_FUCHSIA
}

}  // namespace

SyncFencePool::SyncFencePool(VkDevice device, SyncDeviceClient* syncDevice)
    : ObjectPool<SyncFence>(
          kPoolSizeLimit,
          // createObject
          [device, syncDevice] {
              return SyncFence{
                  .fence = newFence(device),
                  .timelineClient = newTimelineClient(syncDevice),
              };
          },
          // onDestroy
          [device = device](SyncFence& fence) { destroyFence(device, fence.fence); },
          // onRelease
          [device = device](SyncFence& fence) { resetFence(device, fence.fence); }) {}

}  // namespace goldfish_vk
