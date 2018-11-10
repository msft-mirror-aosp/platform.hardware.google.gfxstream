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
#include "Resources.h"

#include "android/base/AlignedBuf.h"

#include <log/log.h>
#include <stdlib.h>

using android::aligned_buf_alloc;
using android::aligned_buf_free;

extern "C" {

#define GOLDFISH_VK_NEW_DISPATCHABLE_FROM_HOST_IMPL(type) \
    type new_from_host_##type(type underlying) { \
        struct goldfish_##type* res = \
            static_cast<goldfish_##type*>(malloc(sizeof(goldfish_##type))); \
        if (!res) { \
            ALOGE("FATAL: Failed to alloc " #type " handle"); \
            abort(); \
        } \
        res->dispatch.magic = HWVULKAN_DISPATCH_MAGIC; \
        res->underlying = underlying; \
        return reinterpret_cast<type>(res); \
    } \

#define GOLDFISH_VK_NEW_TRIVIAL_NON_DISPATCHABLE_FROM_HOST_IMPL(type) \
    type new_from_host_##type(type underlying) { \
        struct goldfish_##type* res = \
            static_cast<goldfish_##type*>(malloc(sizeof(goldfish_##type))); \
        res->underlying = underlying; \
        return reinterpret_cast<type>(res); \
    } \

#define GOLDFISH_VK_AS_GOLDFISH_IMPL(type) \
    struct goldfish_##type* as_goldfish_##type(type toCast) { \
        return reinterpret_cast<goldfish_##type*>(toCast); \
    } \

#define GOLDFISH_VK_GET_HOST_IMPL(type) \
    type get_host_##type(type toUnwrap) { \
        if (!toUnwrap) return VK_NULL_HANDLE; \
        auto as_goldfish = as_goldfish_##type(toUnwrap); \
        return as_goldfish->underlying; \
    } \

#define GOLDFISH_VK_DELETE_GOLDFISH_IMPL(type) \
    void delete_goldfish_##type(type toDelete) { \
        free(as_goldfish_##type(toDelete)); \
    } \

GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_NEW_DISPATCHABLE_FROM_HOST_IMPL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_AS_GOLDFISH_IMPL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_GET_HOST_IMPL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_DELETE_GOLDFISH_IMPL)

GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_AS_GOLDFISH_IMPL)
GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_GET_HOST_IMPL)

GOLDFISH_VK_LIST_TRIVIAL_NON_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_NEW_TRIVIAL_NON_DISPATCHABLE_FROM_HOST_IMPL)
GOLDFISH_VK_LIST_TRIVIAL_NON_DISPATCHABLE_HANDLE_TYPES(GOLDFISH_VK_DELETE_GOLDFISH_IMPL)

// Custom definitions///////////////////////////////////////////////////////////

VkDeviceMemory new_from_host_VkDeviceMemory(VkDeviceMemory mem) {
    struct goldfish_VkDeviceMemory *res =
        (struct goldfish_VkDeviceMemory *)malloc(sizeof(goldfish_VkDeviceMemory));

    if (!res) {
        ALOGE("FATAL: Failed to alloc VkDeviceMemory handle");
        abort();
    }

    memset(res, 0x0, sizeof(goldfish_VkDeviceMemory));

    res->underlying = mem;

    return reinterpret_cast<VkDeviceMemory>(res);
}

void delete_goldfish_VkDeviceMemory(VkDeviceMemory mem) {
    struct goldfish_VkDeviceMemory* goldfish_mem =
        as_goldfish_VkDeviceMemory(mem);

    if (goldfish_mem->ptr) {
        // TODO: unmap the pointer with address space device
        aligned_buf_free(goldfish_mem->ptr);
    }

    free(goldfish_mem);
}

VkResult goldfish_vkAllocateMemory(
    VkDevice,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks*,
    VkDeviceMemory* pMemory) {

    // Assumes pMemory has already been allocated.
    goldfish_VkDeviceMemory* mem = as_goldfish_VkDeviceMemory(*pMemory);

    VkDeviceSize size = pAllocateInfo->allocationSize;

    // This is a strict alignment; we do not expect any
    // actual device to have more stringent requirements
    // than this.
    mem->ptr = (uint8_t*)aligned_buf_alloc(4096, size);
    mem->size = size;

    return VK_SUCCESS;
}

VkResult goldfish_vkMapMemory(
    VkDevice,
    VkDeviceMemory memory,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkMemoryMapFlags,
    void** ppData) {

    goldfish_VkDeviceMemory* mem = as_goldfish_VkDeviceMemory(memory);

    if (!mem->ptr) {
        ALOGE("%s: Did not allocate host pointer for device memory!", __func__);
        abort();
    }

    if (mem->ptr + offset >= mem->ptr + size) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    *ppData = mem->ptr + offset;

    return VK_SUCCESS;
}

void goldfish_vkUnmapMemory(
    VkDevice,
    VkDeviceMemory) {
    // no-op
}

} // extern "C"