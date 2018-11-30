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

#include "VulkanHandleMapping.h"
#include "VulkanHandles.h"
#include "VkEventHandler.h"
#include <memory>

namespace goldfish_vk {

class ResourceTracker : public VkEventHandler {
public:
    ResourceTracker();
    virtual ~ResourceTracker();
    static ResourceTracker* get();
    VulkanHandleMapping* createMapping();
    VulkanHandleMapping* unwrapMapping();
    VulkanHandleMapping* destroyMapping();
    VulkanHandleMapping* defaultMapping();

#define HANDLE_REGISTER_DECL(type) \
    void register_##type(type); \
    void unregister_##type(type); \

    GOLDFISH_VK_LIST_HANDLE_TYPES(HANDLE_REGISTER_DECL)

    void setDeviceInfo(VkDevice device, VkPhysicalDevice physdev, VkPhysicalDeviceProperties props, VkPhysicalDeviceMemoryProperties memProps);
    bool isMemoryTypeHostVisible(VkDevice device, uint32_t typeIndex) const;
    VkDeviceSize getNonCoherentExtendedSize(VkDevice device, VkDeviceSize basicSize) const;
    bool isValidMemoryRange(
        VkDevice device,
        const VkMappedMemoryRange& range) const;
  private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace goldfish_vk
