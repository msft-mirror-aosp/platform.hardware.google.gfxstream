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

#ifdef __cplusplus
#include <algorithm>
#endif

// TODO(b/349066492): this is used as a placeholder extension to inform guest side
// that host is using MoltenVK, it should be removed after external_memory_metal
// extension is implemented.
#ifndef VK_MVK_moltenvk
#define VK_MVK_moltenvk 1
#define VK_MVK_MOLTENVK_SPEC_VERSION 3
#define VK_MVK_MOLTENVK_EXTENSION_NAME "VK_MVK_moltenvk"
#endif  // VK_MVK_moltenvk

// These are internally defined MoltenVK flags for external memory usage
// TODO(b/349066492): They should be removed after being ratified and put under the headers
static const VkExternalMemoryHandleTypeFlagBits VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_KHR =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM;
static const VkExternalMemoryHandleTypeFlagBits VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_KHR =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM;

// VulkanStream features
#define VULKAN_STREAM_FEATURE_NULL_OPTIONAL_STRINGS_BIT (1 << 0)
#define VULKAN_STREAM_FEATURE_IGNORED_HANDLES_BIT (1 << 1)
#define VULKAN_STREAM_FEATURE_SHADER_FLOAT16_INT8_BIT (1 << 2)
#define VULKAN_STREAM_FEATURE_QUEUE_SUBMIT_WITH_COMMANDS_BIT (1 << 3)

#define VK_YCBCR_CONVERSION_DO_NOTHING ((VkSamplerYcbcrConversion)0x1111111111111111)

#ifdef __cplusplus

template<class T, typename F>
bool arrayany(const T* arr, uint32_t begin, uint32_t end, const F& func) {
    const T* e = arr + end;
    return std::find_if(arr + begin, e, func) != e;
}

#define DEFINE_ALIAS_FUNCTION(ORIGINAL_FN, ALIAS_FN) \
template <typename... Args> \
inline auto ALIAS_FN(Args&&... args) -> decltype(ORIGINAL_FN(std::forward<Args>(args)...)) { \
  return ORIGINAL_FN(std::forward<Args>(args)...); \
}

#endif
