// Copyright (C) 2018 The Android Open Source Project
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

#include <hardware/hwvulkan.h>
#include <vulkan/vulkan.h>

#include "VulkanHandles.h"

#include <inttypes.h>

extern "C" {

#define GOLDFISH_VK_DEFINE_DISPATCHABLE_HANDLE_STRUCT(type) \
    struct goldfish_##type { \
        hwvulkan_dispatch_t dispatch; \
        uint64_t underlying; \
    }; \

#define GOLDFISH_VK_DEFINE_TRIVIAL_NON_DISPATCHABLE_HANDLE_STRUCT(type) \
    struct goldfish_##type { \
        uint64_t underlying; \
    }; \

#define GOLDFISH_VK_NEW_FROM_HOST_DECL(type) \
    type new_from_host_##type(type);

#define GOLDFISH_VK_AS_GOLDFISH_DECL(type) \
    struct goldfish_##type* as_goldfish_##type(type);

#define GOLDFISH_VK_GET_HOST_DECL(type) \
    type get_host_##type(type);

#define GOLDFISH_VK_DELETE_GOLDFISH_DECL(type) \
    void delete_goldfish_##type(type);

#define GOLDFISH_VK_IDENTITY_DECL(type) \
    type vk_handle_identity_##type(type);

#define GOLDFISH_VK_NEW_FROM_HOST_U64_DECL(type) \
    type new_from_host_u64_##type(uint64_t);

#define GOLDFISH_VK_GET_HOST_U64_DECL(type) \
    uint64_t get_host_u64_##type(type);

GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_DEFINE_DISPATCHABLE_HANDLE_STRUCT)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_NEW_FROM_HOST_DECL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_AS_GOLDFISH_DECL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_GET_HOST_DECL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_DELETE_GOLDFISH_DECL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_IDENTITY_DECL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_NEW_FROM_HOST_U64_DECL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_GET_HOST_U64_DECL)

GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_NEW_FROM_HOST_DECL)
GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_AS_GOLDFISH_DECL)
GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_GET_HOST_DECL)
GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_DELETE_GOLDFISH_DECL)
GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_IDENTITY_DECL)
GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_NEW_FROM_HOST_U64_DECL)
GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_GET_HOST_U64_DECL)

GOLDFISH_VK_LIST_TRIVIAL_NON_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_DEFINE_TRIVIAL_NON_DISPATCHABLE_HANDLE_STRUCT)

// Custom definitions///////////////////////////////////////////////////////////

VkResult goldfish_vkEnumerateInstanceVersion(
    void* opaque,
    VkResult host_return,
    uint32_t* apiVersion);

VkResult goldfish_vkEnumerateDeviceExtensionProperties(
    void* opaque,
    VkResult host_return,
    VkPhysicalDevice physicalDevice, const char *pLayerName,
    uint32_t *pPropertyCount, VkExtensionProperties *pProperties);

void goldfish_vkGetPhysicalDeviceProperties2(
    void* opaque,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties);

struct goldfish_VkDeviceMemory {
    uint64_t underlying;
    uint8_t* ptr;
    VkDeviceSize size;
    VkDeviceSize mappedSize;
};

VkResult goldfish_vkMapMemory(
    void* opaque,
    VkResult host_return,
    VkDevice device,
    VkDeviceMemory memory,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkMemoryMapFlags flags,
    void** ppData);

void goldfish_vkUnmapMemory(
    void* opaque,
    VkDevice device,
    VkDeviceMemory memory);

void goldfish_unwrap_VkNativeBufferANDROID(
    const VkImageCreateInfo* pCreateInfo,
    VkImageCreateInfo* local_pCreateInfo);

void goldfish_unwrap_vkAcquireImageANDROID_nativeFenceFd(int fd, int* fd_out);

} // extern "C"
