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

extern "C" {

#define GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPESS(f) \
    f(VkInstance) \
    f(VkPhysicalDevice) \
    f(VkDevice) \
    f(VkQueue) \
    f(VkCommandBuffer) \

#define GOLDFISH_VK_DEFINE_DISPATCHABLE_HANDLE_STRUCT(type) \
    struct goldfish_##type { \
        hwvulkan_dispatch_t dispatch; \
        type underlying; \
    }; \

#define GOLDFISH_VK_NEW_FROM_HOST_DECL(type) \
    type new_from_host_##type(type);

#define GOLDFISH_VK_AS_GOLDFISH_DECL(type) \
    struct goldfish_##type* as_goldfish_##type(type);

#define GOLDFISH_VK_GET_HOST_DECL(type) \
    type get_host_##type(type);

#define GOLDFISH_VK_DELETE_GOLDFISH_DECL(type) \
    void delete_goldfish_##type(type);

GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPESS(GOLDFISH_VK_DEFINE_DISPATCHABLE_HANDLE_STRUCT)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPESS(GOLDFISH_VK_NEW_FROM_HOST_DECL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPESS(GOLDFISH_VK_AS_GOLDFISH_DECL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPESS(GOLDFISH_VK_GET_HOST_DECL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPESS(GOLDFISH_VK_DELETE_GOLDFISH_DECL)

} // extern "C"