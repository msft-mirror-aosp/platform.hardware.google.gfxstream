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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "VkGuestMemoryUtils.h"

#include <vector>

namespace gfxstream {
namespace vk {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::ExplainMatchResult;
using ::testing::Field;
using ::testing::Matcher;

MATCHER_P(EqsVkMemoryHeap, expected, "") {
    return ExplainMatchResult(AllOf(Field("size", &VkMemoryHeap::size, Eq(expected.size)),
                                    Field("flags", &VkMemoryHeap::flags, Eq(expected.flags))),
                              arg, result_listener);
}

MATCHER_P(EqsVkMemoryType, expected, "") {
    return ExplainMatchResult(
        AllOf(Field("propertyFlags", &VkMemoryType::propertyFlags, Eq(expected.propertyFlags)),
              Field("heapIndex", &VkMemoryType::heapIndex, Eq(expected.heapIndex))),
        arg, result_listener);
}

MATCHER_P(EqsVkPhysicalDeviceMemoryProperties, expected, "") {
    std::vector<Matcher<VkMemoryHeap>> memoryHeapsMatchers;
    for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; i++) {
        memoryHeapsMatchers.push_back(EqsVkMemoryHeap(expected.memoryHeaps[i]));
    }

    std::vector<Matcher<VkMemoryType>> memoryTypesMatchers;
    for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; i++) {
        memoryTypesMatchers.push_back(EqsVkMemoryType(expected.memoryTypes[i]));
    }

    return ExplainMatchResult(
        AllOf(Field("memoryTypeCount", &VkPhysicalDeviceMemoryProperties::memoryTypeCount,
                    Eq(expected.memoryTypeCount)),
              Field("memoryTypes", &VkPhysicalDeviceMemoryProperties::memoryTypes,
                    ElementsAreArray(memoryTypesMatchers)),
              Field("memoryHeapCount", &VkPhysicalDeviceMemoryProperties::memoryHeapCount,
                    Eq(expected.memoryHeapCount)),
              Field("memoryHeaps", &VkPhysicalDeviceMemoryProperties::memoryHeaps,
                    ElementsAreArray(memoryHeapsMatchers))),
        arg, result_listener);
}

TEST(VkGuestMemoryUtilsTest, ReserveAHardwareBuffer) {
    const VkPhysicalDeviceMemoryProperties hostMemoryProperties = {
        .memoryTypeCount = 2,
        .memoryTypes =
            {
                {
                    .propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    .heapIndex = 0,
                },
                {
                    .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    .heapIndex = 1,
                },
            },
        .memoryHeapCount = 2,
        .memoryHeaps =
            {
                {
                    .size = 0x1000000,
                    .flags = 0,
                },
                {
                    .size = 0x200000,
                    .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
                },
            },
    };

    EmulatedPhysicalDeviceMemoryProperties helper(hostMemoryProperties, 1);

    const VkPhysicalDeviceMemoryProperties expectedGuestMemoryProperties = {
        .memoryTypeCount = 3,
        .memoryTypes =
            {
                {
                    .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    .heapIndex = 1,
                },
                {
                    .propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    .heapIndex = 0,
                },
                {
                    .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    .heapIndex = 1,
                },
            },
        .memoryHeapCount = 2,
        .memoryHeaps =
            {
                {
                    .size = 0x1000000,
                    .flags = 0,
                },
                {
                    .size = 0x200000,
                    .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
                },
            },
    };

    const auto actualGuestMemoryProperties = helper.getGuestMemoryProperties();
    EXPECT_THAT(actualGuestMemoryProperties,
                EqsVkPhysicalDeviceMemoryProperties(expectedGuestMemoryProperties));
}

}  // namespace
}  // namespace vk
}  // namespace gfxstream
