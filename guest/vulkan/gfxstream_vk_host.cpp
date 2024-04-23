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

#include <log/log.h>

#include <mutex>

#include "HostConnection.h"
#include "ProcessPipe.h"
#include "ResourceTracker.h"
#include "VkEncoder.h"
#include "func_table.h"

namespace {

static HostConnection* GetGfxstreamVulkanIcdHostConnection();

static gfxstream::vk::VkEncoder* GetGfxstreamVulkanIcdVkEncoder(HostConnection* con) {
    return con->vkEncoder();
}

gfxstream::vk::ResourceTracker::ThreadingCallbacks sThreadingCallbacks = {
    .hostConnectionGetFunc = GetGfxstreamVulkanIcdHostConnection,
    .vkEncoderGetFunc = GetGfxstreamVulkanIcdVkEncoder,
};

static HostConnection* GetGfxstreamVulkanIcdHostConnection() {
    return HostConnection::getOrCreate(kCapsetGfxStreamVulkan);
}

VkResult MaybeDoPerProcessInit() {
    auto* hostConnection = GetGfxstreamVulkanIcdHostConnection();
    if (!hostConnection) {
        ALOGE("vulkan: Failed to get host connection\n");
        return VK_ERROR_DEVICE_LOST;
    }

    auto* resourceTracker = gfxstream::vk::ResourceTracker::get();

    uint32_t noRenderControlEnc = 0;
    resourceTracker->setupCaps(noRenderControlEnc);
    // Legacy goldfish path: could be deleted once goldfish not used guest-side.
    if (!noRenderControlEnc) {
        // Implicitly sets up sequence number
        ExtendedRCEncoderContext* rcEnc = hostConnection->rcEncoder();
        if (!rcEnc) {
            ALOGE("vulkan: Failed to get renderControl encoder context\n");
            return VK_ERROR_DEVICE_LOST;
        }

        resourceTracker->setupFeatures(rcEnc->featureInfo_const());
    }

    resourceTracker->setThreadingCallbacks(sThreadingCallbacks);
    resourceTracker->setSeqnoPtr(getSeqnoPtrForProcess());

    auto* vkEnc = GetGfxstreamVulkanIcdVkEncoder(hostConnection);
    if (!vkEnc) {
        ALOGE("vulkan: Failed to get Vulkan encoder\n");
        return VK_ERROR_DEVICE_LOST;
    }

    return VK_SUCCESS;
}

static void ResetProcess() {
    HostConnection::exit();
    processPipeRestart();
}

#define VK_HOST_CONNECTION(ret)                                                    \
    HostConnection* hostCon = GetGfxstreamVulkanIcdHostConnection();               \
    if (hostCon == nullptr) {                                                      \
        ALOGE("Gfxstream Vulkan ICD: Failed to get HostConnection.");              \
        return ret;                                                                \
    }                                                                              \
                                                                                   \
    gfxstream::vk::VkEncoder* vkEnc = hostCon->vkEncoder();                        \
    if (!vkEnc) {                                                                  \
        ALOGE("Gfxstream Vulkan ICD: Failed to get VkEncoder.");                   \
        return ret;                                                                \
    }

VKAPI_ATTR
VkResult EnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    VkResult result = MaybeDoPerProcessInit();
    if (result != VK_SUCCESS) {
        return result;
    }

    VK_HOST_CONNECTION(VK_ERROR_DEVICE_LOST)
    return vkEnc->vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties, true /* do lock */);
}

VKAPI_ATTR
VkResult EnumerateInstanceExtensionProperties(const char* layer_name, uint32_t* count, VkExtensionProperties* properties) {
    VkResult result = MaybeDoPerProcessInit();
    if (result != VK_SUCCESS) {
        return result;
    }

    VK_HOST_CONNECTION(VK_ERROR_DEVICE_LOST)

    if (layer_name) {
        ALOGW(
            "Driver vkEnumerateInstanceExtensionProperties shouldn't be called "
            "with a layer name ('%s')",
            layer_name);
    }

    return gfxstream::vk::ResourceTracker::get()->on_vkEnumerateInstanceExtensionProperties(
        vkEnc, VK_SUCCESS, layer_name, count, properties);
}

VKAPI_ATTR
VkResult CreateInstance(const VkInstanceCreateInfo* create_info,
                        const VkAllocationCallbacks* allocator,
                        VkInstance* out_instance) {
    VkResult result = MaybeDoPerProcessInit();
    if (result != VK_SUCCESS) {
        return result;
    }

    VK_HOST_CONNECTION(VK_ERROR_DEVICE_LOST)
    return vkEnc->vkCreateInstance(create_info, nullptr, out_instance, true /* do lock */);
}

VKAPI_ATTR
void DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    VK_HOST_CONNECTION()
    vkEnc->vkDestroyInstance(instance, pAllocator, true /* do lock */);

    ResetProcess();
}

static
PFN_vkVoidFunction GetDeviceProcAddr(VkDevice device, const char* name) {
    VK_HOST_CONNECTION(nullptr)

    if (!strcmp(name, "vkGetDeviceProcAddr")) {
        return (PFN_vkVoidFunction)(GetDeviceProcAddr);
    }
    return (PFN_vkVoidFunction)(gfxstream::vk::goldfish_vulkan_get_device_proc_address(device, name));
}

VKAPI_ATTR
PFN_vkVoidFunction GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!strcmp(name, "vkEnumerateInstanceLayerProperties")) {
        return (PFN_vkVoidFunction)EnumerateInstanceLayerProperties;
    }
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties")) {
        return (PFN_vkVoidFunction)EnumerateInstanceExtensionProperties;
    }
    if (!strcmp(name, "vkCreateInstance")) {
        return (PFN_vkVoidFunction)CreateInstance;
    }
    if (!strcmp(name, "vkDestroyInstance")) {
        return (PFN_vkVoidFunction)DestroyInstance;
    }
    if (!strcmp(name, "vkGetDeviceProcAddr")) {
        return (PFN_vkVoidFunction)(GetDeviceProcAddr);
    }
    return (PFN_vkVoidFunction)(gfxstream::vk::goldfish_vulkan_get_instance_proc_address(instance, name));
}

} // namespace

extern "C" __attribute__((visibility("default"))) PFN_vkVoidFunction
vk_icdGetInstanceProcAddr(VkInstance instance, const char* name) {
    return GetInstanceProcAddr(instance, name);
}

extern "C" __attribute__((visibility("default"))) VkResult
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion) {
    *pSupportedVersion = std::min(*pSupportedVersion, 3u);
    return VK_SUCCESS;
}
