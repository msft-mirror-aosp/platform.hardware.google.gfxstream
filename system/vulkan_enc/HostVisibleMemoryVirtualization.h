// Copyright (C) 2018 The Android Open Source Project
// Copyright (C) 2018 Google Inc.
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

#include <vulkan/vulkan.h>

namespace goldfish_vk {

struct HostVisibleMemoryVirtualizationInfo {
    bool supported;

    VkPhysicalDevice physicalDevice;

    VkPhysicalDeviceMemoryProperties hostMemoryProperties;
    VkPhysicalDeviceMemoryProperties guestMemoryProperties;

    uint32_t memoryTypeIndexMappingToHost[VK_MAX_MEMORY_TYPES];
    uint32_t memoryHeapIndexMappingToHost[VK_MAX_MEMORY_TYPES];

    uint32_t memoryTypeIndexMappingFromHost[VK_MAX_MEMORY_TYPES];
    uint32_t memoryHeapIndexMappingFromHost[VK_MAX_MEMORY_TYPES];

    bool memoryTypeBitsShouldAdvertiseBoth[VK_MAX_MEMORY_TYPES];
};

bool canFitVirtualHostVisibleMemoryInfo(
    const VkPhysicalDeviceMemoryProperties* memoryProperties);

void initHostVisibleMemoryVirtualizationInfo(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceMemoryProperties* memoryProperties,
    HostVisibleMemoryVirtualizationInfo* info_out);

} // namespace goldfish_vk