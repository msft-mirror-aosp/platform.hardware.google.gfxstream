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
#include <hardware/hwvulkan.h>

#include <log/log.h>

#include <errno.h>
#include <string.h>

#include "HostConnection.h"
#include "VkEncoder.h"

#include "func_table.h"

namespace {

int OpenDevice(const hw_module_t* module, const char* id, hw_device_t** device);

hw_module_methods_t goldfish_vulkan_module_methods = {
    .open = OpenDevice
};

extern "C" __attribute__((visibility("default"))) hwvulkan_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = HWVULKAN_HARDWARE_MODULE_ID,
        .name = "Goldfish Vulkan Driver",
        .author = "The Android Open Source Project",
        .methods = &goldfish_vulkan_module_methods,
    },
};

int CloseDevice(struct hw_device_t* /*device*/) {
    // nothing to do - opening a device doesn't allocate any resources
    return 0;
}

#define VK_HOST_CONNECTION \
    HostConnection *hostCon = HostConnection::get(); \
    if (!hostCon) { \
        ALOGE("vulkan: Failed to get host connection\n"); \
        return VK_ERROR_DEVICE_LOST; \
    } \
    ExtendedRCEncoderContext *rcEnc = hostCon->rcEncoder(); \
    if (!rcEnc) { \
        ALOGE("vulkan: Failed to get renderControl encoder context\n"); \
        return VK_ERROR_DEVICE_LOST; \
    } \
    VkEncoder *vkEnc = hostCon->vkEncoder(); \
    if (!vkEnc) { \
        ALOGE("vulkan: Failed to get Vulkan encoder\n"); \
        return VK_ERROR_DEVICE_LOST; \
    } \

VKAPI_ATTR
VkResult EnumerateInstanceExtensionProperties(
    const char* layer_name,
    uint32_t* count,
    VkExtensionProperties* properties) {

    ALOGD("%s: call from goldfish_vulkan\n", __func__);
    VK_HOST_CONNECTION;

    ALOGD("%s: yolo this call as a test.\n", __func__);
    VkResult res = vkEnc->vkEnumerateInstanceExtensionProperties(nullptr, count, properties);
    ALOGD("%s: yolo done. res == VK_SUCCESS? %d count: %u\n",
          __func__, res == VK_SUCCESS,
          *count);

    if (layer_name) {
        ALOGW(
            "Driver vkEnumerateInstanceExtensionProperties shouldn't be called "
            "with a layer name ('%s')",
            layer_name);
    }

    return VK_SUCCESS;
}

VKAPI_ATTR
VkResult CreateInstance(const VkInstanceCreateInfo* create_info,
                        const VkAllocationCallbacks* allocator,
                        VkInstance* out_instance) {
    ALOGD("%s: goldfish vkCreateInstance\n", __func__);
    VK_HOST_CONNECTION;

    VkResult res = vkEnc->vkCreateInstance(create_info, nullptr, out_instance);

    return res;
}

VKAPI_ATTR
PFN_vkVoidFunction GetInstanceProcAddr(VkInstance instance, const char* name) {
    (void)instance;

    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties")) {
        return (PFN_vkVoidFunction)EnumerateInstanceExtensionProperties;
    }
    if (!strcmp(name, "vkCreateInstance")) {
        return (PFN_vkVoidFunction)CreateInstance;
    }
    return (PFN_vkVoidFunction)(goldfish_vk::goldfish_vulkan_get_proc_address(name));
}

hwvulkan_device_t goldfish_vulkan_device = {
    .common = {
        .tag = HARDWARE_DEVICE_TAG,
        .version = HWVULKAN_DEVICE_API_VERSION_0_1,
        .module = &HAL_MODULE_INFO_SYM.common,
        .close = CloseDevice,
    },
    .EnumerateInstanceExtensionProperties = EnumerateInstanceExtensionProperties,
    .CreateInstance = CreateInstance,
    .GetInstanceProcAddr = GetInstanceProcAddr,
};

int OpenDevice(const hw_module_t* /*module*/,
               const char* id,
               hw_device_t** device) {
    if (strcmp(id, HWVULKAN_DEVICE_0) == 0) {
        *device = &goldfish_vulkan_device.common;
        return 0;
    }
    return -ENOENT;
}

} // namespace
