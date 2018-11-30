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
#include "Validation.h"

#include "Resources.h"

namespace goldfish_vk {

static bool is_range_good(const VkMappedMemoryRange& range) {
    const goldfish_VkDeviceMemory* mem =
        as_goldfish_VkDeviceMemory(range.memory);

    if (!mem || !mem->ptr) {
        return false;
    }

    VkDeviceSize offset = range.offset;
    VkDeviceSize size = range.size;

    if (size == VK_WHOLE_SIZE) {
        return offset <= mem->mappedSize;
    };

    return offset + size <= mem->mappedSize;
}

VkResult Validation::on_vkFlushMappedMemoryRanges(
    void*,
    VkResult,
    VkDevice,
    uint32_t memoryRangeCount,
    const VkMappedMemoryRange* pMemoryRanges) {

    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        if (!is_range_good(pMemoryRanges[i])) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    return VK_SUCCESS;
}

VkResult Validation::on_vkInvalidateMappedMemoryRanges(
    void*,
    VkResult,
    VkDevice,
    uint32_t memoryRangeCount,
    const VkMappedMemoryRange* pMemoryRanges) {

    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        if (!is_range_good(pMemoryRanges[i])) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    return VK_SUCCESS;
}

} // namespace goldfish_vk
