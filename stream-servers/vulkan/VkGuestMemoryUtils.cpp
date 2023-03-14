// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "VkGuestMemoryUtils.h"

#include <algorithm>
#include <limits>

#include "host-common/GfxstreamFatalError.h"

namespace gfxstream {
namespace vk {
namespace {

static constexpr const uint32_t kInvalidMemoryTypeIndex = std::numeric_limits<uint32_t>::max();

}  // namespace

EmulatedPhysicalDeviceMemoryProperties::EmulatedPhysicalDeviceMemoryProperties(
    const VkPhysicalDeviceMemoryProperties& hostMemoryProperties,
    const uint32_t hostColorBufferMemoryTypeIndex) {
    mHostMemoryProperties = hostMemoryProperties;
    mGuestMemoryProperties = hostMemoryProperties;
    std::fill_n(mGuestToHostMemoryTypeIndexMap, VK_MAX_MEMORY_TYPES, kInvalidMemoryTypeIndex);
    std::fill_n(mHostToGuestMemoryTypeIndexMap, VK_MAX_MEMORY_TYPES, kInvalidMemoryTypeIndex);

    // Reserve the first memory type (index 0) for AHB backed buffers and images so
    // that the host can control its memory properties. This ensures that the guest
    // only sees `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT` and will not try to map the
    // memory.
    if (mHostMemoryProperties.memoryTypeCount == VK_MAX_MEMORY_TYPES) {
        GFXSTREAM_ABORT(emugl::FatalError(emugl::ABORT_REASON_OTHER))
            << "Unable to create reserved AHB memory type.";
    }
    for (int32_t i = mHostMemoryProperties.memoryTypeCount; i >= 0; i--) {
        const uint32_t hostIndex = i;
        const uint32_t guestIndex = i + 1;
        mGuestMemoryProperties.memoryTypes[guestIndex] =
            mGuestMemoryProperties.memoryTypes[hostIndex];
        mGuestToHostMemoryTypeIndexMap[guestIndex] = hostIndex;
        mHostToGuestMemoryTypeIndexMap[hostIndex] = guestIndex;
    }
    mGuestMemoryProperties.memoryTypeCount += 1;
    mGuestMemoryProperties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    mGuestMemoryProperties.memoryTypes[0].heapIndex =
        mHostMemoryProperties.memoryTypes[hostColorBufferMemoryTypeIndex].heapIndex;
}

std::optional<EmulatedPhysicalDeviceMemoryProperties::HostMemoryInfo>
EmulatedPhysicalDeviceMemoryProperties::getHostMemoryInfoFromHostMemoryTypeIndex(
    uint32_t hostMemoryTypeIndex) const {
    if (hostMemoryTypeIndex >= mHostMemoryProperties.memoryTypeCount) {
        return std::nullopt;
    }

    return HostMemoryInfo{
        .index = hostMemoryTypeIndex,
        .memoryType = mHostMemoryProperties.memoryTypes[hostMemoryTypeIndex],
    };
}

std::optional<EmulatedPhysicalDeviceMemoryProperties::HostMemoryInfo>
EmulatedPhysicalDeviceMemoryProperties::getHostMemoryInfoFromGuestMemoryTypeIndex(
    uint32_t guestMemoryTypeIndex) const {
    if (guestMemoryTypeIndex >= mGuestMemoryProperties.memoryTypeCount) {
        return std::nullopt;
    }

    uint32_t hostMemoryTypeIndex = mGuestToHostMemoryTypeIndexMap[guestMemoryTypeIndex];
    if (hostMemoryTypeIndex == kInvalidMemoryTypeIndex) {
        return std::nullopt;
    }

    return getHostMemoryInfoFromHostMemoryTypeIndex(hostMemoryTypeIndex);
}

void EmulatedPhysicalDeviceMemoryProperties::transformToGuestMemoryRequirements(
    VkMemoryRequirements* memoryRequirements) const {
    uint32_t guestMemoryTypeBits = 0;

    const uint32_t hostMemoryTypeBits = memoryRequirements->memoryTypeBits;
    for (uint32_t hostMemoryTypeIndex = 0;
         hostMemoryTypeIndex < mHostMemoryProperties.memoryTypeCount; hostMemoryTypeIndex++) {
        if (!(hostMemoryTypeBits & (1u << hostMemoryTypeIndex))) {
            continue;
        }

        uint32_t guestMemoryTypeIndex = mHostToGuestMemoryTypeIndexMap[hostMemoryTypeIndex];
        if (guestMemoryTypeIndex == kInvalidMemoryTypeIndex) {
            continue;
        }

        guestMemoryTypeBits |= (1u << guestMemoryTypeIndex);
    }

    memoryRequirements->memoryTypeBits = guestMemoryTypeBits;
}

}  // namespace vk
}  // namespace gfxstream
