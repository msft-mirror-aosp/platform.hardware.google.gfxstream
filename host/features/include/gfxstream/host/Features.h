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
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <string>
#include <map>

namespace gfxstream {
namespace host {

struct FeatureInfo;

using FeatureMap = std::map<std::string, FeatureInfo*>;

struct FeatureInfo {
    FeatureInfo(const FeatureInfo& rhs) = default;

    FeatureInfo(const char* name,
                const char* description,
                FeatureMap* map) :
            name(name),
            description(description),
            enabled(false),
            reason("Default value") {
        if (map) {
            (*map)[std::string(name)] = this;
        }
    }

    ~FeatureInfo() = default;

    std::string name;
    std::string description;
    bool enabled;
    std::string reason;
};

struct FeatureSet {
    FeatureSet() = default;

    FeatureSet(const FeatureSet& rhs);
    FeatureSet& operator=(const FeatureSet& rhs);

    FeatureMap map;

    FeatureInfo AsyncComposeSupport = {
        "AsyncComposeSupport",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo ExternalBlob = {
        "ExternalBlob",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo SystemBlob = {
        "SystemBlob",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo GlAsyncSwap = {
        "GlAsyncSwap",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo GlDirectMem = {
        "GlDirectMem",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo GlDma = {
        "GlDma",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo GlDma2 = {
        "GlDma2",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo GlPipeChecksum = {
        "GlPipeChecksum",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo GlesDynamicVersion = {
        "GlesDynamicVersion",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo GrallocSync = {
        "GrallocSync",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo GuestUsesAngle = {
        "GuestUsesAngle",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo HasSharedSlotsHostMemoryAllocator = {
        "HasSharedSlotsHostMemoryAllocator",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo HostComposition = {
        "HostComposition",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo HwcMultiConfigs = {
        "HwcMultiConfigs",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo Minigbm = {
        "Minigbm",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo NativeTextureDecompression = {
        "NativeTextureDecompression",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo NoDelayCloseColorBuffer = {
        "NoDelayCloseColorBuffer",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo PlayStoreImage = {
        "PlayStoreImage",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo RefCountPipe = {
        "RefCountPipe",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo VirtioGpuFenceContexts = {
        "VirtioGpuFenceContexts",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo VirtioGpuNativeSync = {
        "VirtioGpuNativeSync",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo VirtioGpuNext = {
        "VirtioGpuNext",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo VulkanAllocateDeviceMemoryOnly = {
        "VulkanAllocateDeviceMemoryOnly",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo VulkanAllocateHostMemory = {
        "VulkanAllocateHostMemory",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo VulkanBatchedDescriptorSetUpdate = {
        "VulkanBatchedDescriptorSetUpdate",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo VulkanIgnoredHandles = {
        "VulkanIgnoredHandles",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo VulkanNativeSwapchain = {
        "VulkanNativeSwapchain",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo VulkanNullOptionalStrings = {
        "VulkanNullOptionalStrings",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo VulkanQueueSubmitWithCommands = {
        "VulkanQueueSubmitWithCommands",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo VulkanShaderFloat16Int8 = {
        "VulkanShaderFloat16Int8",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo VulkanSnapshots = {
        "VulkanSnapshots",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo Vulkan = {
        "Vulkan",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo Yuv420888ToNv21 = {
        "Yuv420888ToNv21",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
    FeatureInfo YuvCache = {
        "YuvCache",
        "Default description: consider contributing a description if you see this!",
        &map,
    };
};

#define GFXSTREAM_SET_FEATURE_ON_CONDITION(set, feature, condition) \
    do                                                              \
    {                                                               \
        {                                                           \
            (set)->feature.enabled = condition;                     \
            (set)->feature.reason = #condition;                     \
        }                                                           \
    } while (0)

}  // namespace host
}  // namespace gfxstream
