// Copyright 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either expresso or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <vulkan/vulkan.h>

#include <memory>
#include <mutex>
#include <vector>
#include <unordered_set>

#include "vulkan/cereal/common/goldfish_vk_dispatch.h"

namespace gfxstream {
namespace vk {

// TODO: Support VK_AMD_buffer_marker.
class DeviceLostHelper {
   public:
    DeviceLostHelper() : mEnabled(false) {};

    DeviceLostHelper(const DeviceLostHelper&) = delete;
    DeviceLostHelper& operator=(const DeviceLostHelper&) = delete;

    DeviceLostHelper(DeviceLostHelper&&) = delete;
    DeviceLostHelper& operator=(const DeviceLostHelper&&) = delete;

    void enableWithNvidiaDeviceDiagnosticCheckpoints();

    void addNeededDeviceExtensions(std::vector<const char*>* deviceExtensions);

    void onBeginCommandBuffer(const VkCommandBuffer& commandBuffer, const VulkanDispatch* vk);
    void onEndCommandBuffer(const VkCommandBuffer& commandBuffer, const VulkanDispatch* vk);

    void onResetCommandBuffer(const VkCommandBuffer& commandBuffer);
    void onFreeCommandBuffer(const VkCommandBuffer& commandBuffer);

    struct DeviceWithQueues {
        VkDevice device;
        const VulkanDispatch* deviceDispatch;
        std::vector<VkQueue> queues;
    };
    void onDeviceLost(const std::vector<DeviceWithQueues>& devicesWithQueues);

   private:
    enum class MarkerType { kBegin, kEnd };

    struct CheckpointMarker {
        VkCommandBuffer commandBuffer;
        MarkerType type;
    };

    struct CheckpointMarkerEq {
        bool operator()(const CheckpointMarker& lhs, const CheckpointMarker& rhs) const {
            return lhs.commandBuffer == rhs.commandBuffer && lhs.type == rhs.type;
        }
    };

    struct CheckpointMarkerHash {
        size_t operator()(const CheckpointMarker& marker) const {
            std::size_t h1 = (std::size_t)(marker.commandBuffer);
            std::size_t h2 = (std::size_t)(marker.type);
            return h1 ^ (h2 << 1);
        }
    };

    const void* createMarkerForCommandBuffer(const VkCommandBuffer& commandBuffer, MarkerType type);
    void removeMarkersForCommandBuffer(const VkCommandBuffer& commandBuffer);

    bool mEnabled = false;

    std::mutex mMarkersMutex;
    std::unordered_set<CheckpointMarker, CheckpointMarkerHash, CheckpointMarkerEq> mMarkers;
};

}  // namespace vk
}  // namespace gfxstream
