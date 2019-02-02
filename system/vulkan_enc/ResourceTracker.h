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
#include <memory>

struct EmulatorFeatureInfo;

namespace goldfish_vk {

class ResourceTracker {
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

    VkResult on_vkEnumerateInstanceVersion(
        void* context,
        VkResult input_result,
        uint32_t* apiVersion);

    VkResult on_vkEnumerateInstanceExtensionProperties(
        void* context,
        VkResult input_result,
        const char* pLayerName,
        uint32_t* pPropertyCount,
        VkExtensionProperties* pProperties);

    VkResult on_vkEnumerateDeviceExtensionProperties(
        void* context,
        VkResult input_result,
        VkPhysicalDevice physicalDevice,
        const char* pLayerName,
        uint32_t* pPropertyCount,
        VkExtensionProperties* pProperties);

    void on_vkGetPhysicalDeviceMemoryProperties(
        void* context,
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties* pMemoryProperties);
    void on_vkGetPhysicalDeviceMemoryProperties2(
        void* context,
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties2* pMemoryProperties);
    void on_vkGetPhysicalDeviceMemoryProperties2KHR(
        void* context,
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties2* pMemoryProperties);

    VkResult on_vkCreateInstance(
        void* context,
        VkResult input_result,
        const VkInstanceCreateInfo* createInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance);
    VkResult on_vkCreateDevice(
        void* context,
        VkResult input_result,
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice);
    void on_vkDestroyDevice_pre(
        void* context,
        VkDevice device,
        const VkAllocationCallbacks* pAllocator);

    VkResult on_vkAllocateMemory(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkMemoryAllocateInfo* pAllocateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDeviceMemory* pMemory);
    void on_vkFreeMemory(
        void* context,
        VkDevice device,
        VkDeviceMemory memory,
        const VkAllocationCallbacks* pAllocator);

    VkResult on_vkMapMemory(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkDeviceMemory memory,
        VkDeviceSize offset,
        VkDeviceSize size,
        VkMemoryMapFlags,
        void** ppData);

    void on_vkUnmapMemory(
        void* context,
        VkDevice device,
        VkDeviceMemory memory);

    void unwrap_VkNativeBufferANDROID(
        const VkImageCreateInfo* pCreateInfo,
        VkImageCreateInfo* local_pCreateInfo);
    void unwrap_vkAcquireImageANDROID_nativeFenceFd(int fd, int* fd_out);

    VkResult on_vkMapMemoryIntoAddressSpaceGOOGLE_pre(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkDeviceMemory memory,
        uint64_t* pAddress);
    VkResult on_vkMapMemoryIntoAddressSpaceGOOGLE(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkDeviceMemory memory,
        uint64_t* pAddress);

    bool isMemoryTypeHostVisible(VkDevice device, uint32_t typeIndex) const;
    uint8_t* getMappedPointer(VkDeviceMemory memory);
    VkDeviceSize getMappedSize(VkDeviceMemory memory);
    VkDeviceSize getNonCoherentExtendedSize(VkDevice device, VkDeviceSize basicSize) const;
    bool isValidMemoryRange(const VkMappedMemoryRange& range) const;
    void setupFeatures(const EmulatorFeatureInfo* features);
    bool usingDirectMapping() const;
    void deviceMemoryTransform_tohost(
        VkDeviceMemory* memory, uint32_t memoryCount,
        VkDeviceSize* offset, uint32_t offsetCount,
        VkDeviceSize* size, uint32_t sizeCount,
        uint32_t* typeIndex, uint32_t typeIndexCount,
        uint32_t* typeBits, uint32_t typeBitsCount);
    void deviceMemoryTransform_fromhost(
        VkDeviceMemory* memory, uint32_t memoryCount,
        VkDeviceSize* offset, uint32_t offsetCount,
        VkDeviceSize* size, uint32_t sizeCount,
        uint32_t* typeIndex, uint32_t typeIndexCount,
        uint32_t* typeBits, uint32_t typeBitsCount);

    uint32_t getApiVersionFromInstance(VkInstance instance) const;
    uint32_t getApiVersionFromDevice(VkDevice device) const;
    bool hasInstanceExtension(VkInstance instance, const std::string& name) const;
    bool hasDeviceExtension(VkDevice instance, const std::string& name) const;

  private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace goldfish_vk