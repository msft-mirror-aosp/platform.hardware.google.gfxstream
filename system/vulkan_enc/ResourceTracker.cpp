/// Copyright (C) 2018 The Android Open Source Project
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

#include "ResourceTracker.h"

#include "../OpenglSystemCommon/EmulatorFeatureInfo.h"

#ifdef VK_USE_PLATFORM_ANDROID_KHR

typedef uint32_t zx_handle_t;
#define ZX_HANDLE_INVALID         ((zx_handle_t)0)
void zx_handle_close(zx_handle_t) { }

#include "AndroidHardwareBuffer.h"

#endif // VK_USE_PLATFORM_ANDROID_KHR

#ifdef VK_USE_PLATFORM_FUCHSIA

typedef uint32_t AHardwareBuffer;
void AHardwareBuffer_release(AHardwareBuffer*) { }

#include <fuchsia/hardware/goldfish/c/fidl.h>
#include <lib/fzl/fdio.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>

#endif // VK_USE_PLATFORM_FUCHSIA

#include "HostVisibleMemoryVirtualization.h"
#include "Resources.h"
#include "VkEncoder.h"

#include "android/base/AlignedBuf.h"
#include "android/base/synchronization/AndroidLock.h"

#include "gralloc_cb.h"
#include "goldfish_address_space.h"
#include "goldfish_vk_private_defs.h"
#include "vk_util.h"

#include <string>
#include <unordered_map>
#include <set>

#include <vndk/hardware_buffer.h>
#include <log/log.h>
#include <stdlib.h>
#include <sync/sync.h>

#define RESOURCE_TRACKER_DEBUG 0

#if RESOURCE_TRACKER_DEBUG
#undef D
#define D(fmt,...) ALOGD("%s: " fmt, __func__, ##__VA_ARGS__);
#else
#ifndef D
#define D(fmt,...)
#endif
#endif

using android::aligned_buf_alloc;
using android::aligned_buf_free;
using android::base::guest::AutoLock;
using android::base::guest::Lock;

namespace goldfish_vk {

namespace {

template<typename T>
bool GetImportHandle(const void* pNext, VkStructureType import_type,
                     uint32_t bit, uint32_t* pHandle) {
    while (pNext) {
        auto info = static_cast<const T*>(pNext);
        if (info->sType == import_type && info->handleType & bit) {
            *pHandle = info->handle;
            return true;
        }
        pNext = info->pNext;
    }
    return false;
}

template<typename T>
bool HasExportBit(const void* pNext, VkStructureType export_type, uint32_t bit) {
    while (pNext) {
        auto info = static_cast<const T*>(pNext);
        if (info->sType == export_type && info->handleTypes & bit) {
            return true;
        }
        pNext = info->pNext;
    }
    return false;
}

} // namespace

#define MAKE_HANDLE_MAPPING_FOREACH(type_name, map_impl, map_to_u64_impl, map_from_u64_impl) \
    void mapHandles_##type_name(type_name* handles, size_t count) override { \
        for (size_t i = 0; i < count; ++i) { \
            map_impl; \
        } \
    } \
    void mapHandles_##type_name##_u64(const type_name* handles, uint64_t* handle_u64s, size_t count) override { \
        for (size_t i = 0; i < count; ++i) { \
            map_to_u64_impl; \
        } \
    } \
    void mapHandles_u64_##type_name(const uint64_t* handle_u64s, type_name* handles, size_t count) override { \
        for (size_t i = 0; i < count; ++i) { \
            map_from_u64_impl; \
        } \
    } \

#define DEFINE_RESOURCE_TRACKING_CLASS(class_name, impl) \
class class_name : public VulkanHandleMapping { \
public: \
    virtual ~class_name() { } \
    GOLDFISH_VK_LIST_HANDLE_TYPES(impl) \
}; \

#define CREATE_MAPPING_IMPL_FOR_TYPE(type_name) \
    MAKE_HANDLE_MAPPING_FOREACH(type_name, \
        handles[i] = new_from_host_##type_name(handles[i]); ResourceTracker::get()->register_##type_name(handles[i]);, \
        handle_u64s[i] = (uint64_t)new_from_host_##type_name(handles[i]), \
        handles[i] = (type_name)new_from_host_u64_##type_name(handle_u64s[i]); ResourceTracker::get()->register_##type_name(handles[i]);)

#define UNWRAP_MAPPING_IMPL_FOR_TYPE(type_name) \
    MAKE_HANDLE_MAPPING_FOREACH(type_name, \
        handles[i] = get_host_##type_name(handles[i]), \
        handle_u64s[i] = (uint64_t)get_host_u64_##type_name(handles[i]), \
        handles[i] = (type_name)get_host_##type_name((type_name)handle_u64s[i]))

#define DESTROY_MAPPING_IMPL_FOR_TYPE(type_name) \
    MAKE_HANDLE_MAPPING_FOREACH(type_name, \
        ResourceTracker::get()->unregister_##type_name(handles[i]); delete_goldfish_##type_name(handles[i]), \
        (void)handle_u64s[i]; delete_goldfish_##type_name(handles[i]), \
        (void)handles[i]; delete_goldfish_##type_name((type_name)handle_u64s[i]))

DEFINE_RESOURCE_TRACKING_CLASS(CreateMapping, CREATE_MAPPING_IMPL_FOR_TYPE)
DEFINE_RESOURCE_TRACKING_CLASS(UnwrapMapping, UNWRAP_MAPPING_IMPL_FOR_TYPE)
DEFINE_RESOURCE_TRACKING_CLASS(DestroyMapping, DESTROY_MAPPING_IMPL_FOR_TYPE)

class ResourceTracker::Impl {
public:
    Impl() = default;
    CreateMapping createMapping;
    UnwrapMapping unwrapMapping;
    DestroyMapping destroyMapping;
    DefaultHandleMapping defaultMapping;

#define HANDLE_DEFINE_TRIVIAL_INFO_STRUCT(type) \
    struct type##_Info { \
        uint32_t unused; \
    }; \

    GOLDFISH_VK_LIST_TRIVIAL_HANDLE_TYPES(HANDLE_DEFINE_TRIVIAL_INFO_STRUCT)

    struct VkInstance_Info {
        uint32_t highestApiVersion;
        std::set<std::string> enabledExtensions;
        // Fodder for vkEnumeratePhysicalDevices.
        std::vector<VkPhysicalDevice> physicalDevices;
    };

    struct VkDevice_Info {
        VkPhysicalDevice physdev;
        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceMemoryProperties memProps;
        HostMemAlloc hostMemAllocs[VK_MAX_MEMORY_TYPES] = {};
        uint32_t apiVersion;
        std::set<std::string> enabledExtensions;
    };

    struct VkDeviceMemory_Info {
        VkDeviceSize allocationSize = 0;
        VkDeviceSize mappedSize = 0;
        uint8_t* mappedPtr = nullptr;
        uint32_t memoryTypeIndex = 0;
        bool virtualHostVisibleBacking = false;
        bool directMapped = false;
        GoldfishAddressSpaceBlock*
            goldfishAddressSpaceBlock = nullptr;
        SubAlloc subAlloc;
        AHardwareBuffer* ahw = nullptr;
        zx_handle_t vmoHandle = ZX_HANDLE_INVALID;
    };

    // custom guest-side structs for images/buffers because of AHardwareBuffer :((
    struct VkImage_Info {
        VkDevice device;
        VkImageCreateInfo createInfo;
        VkDeviceMemory currentBacking = VK_NULL_HANDLE;
        VkDeviceSize currentBackingOffset = 0;
        VkDeviceSize currentBackingSize = 0;
    };

    struct VkBuffer_Info {
        VkDevice device;
        VkBufferCreateInfo createInfo;
        VkDeviceMemory currentBacking = VK_NULL_HANDLE;
        VkDeviceSize currentBackingOffset = 0;
        VkDeviceSize currentBackingSize = 0;
    };

#define HANDLE_REGISTER_IMPL_IMPL(type) \
    std::unordered_map<type, type##_Info> info_##type; \
    void register_##type(type obj) { \
        AutoLock lock(mLock); \
        info_##type[obj] = type##_Info(); \
    } \

#define HANDLE_UNREGISTER_IMPL_IMPL(type) \
    void unregister_##type(type obj) { \
        AutoLock lock(mLock); \
        info_##type.erase(obj); \
    } \

    GOLDFISH_VK_LIST_HANDLE_TYPES(HANDLE_REGISTER_IMPL_IMPL)
    GOLDFISH_VK_LIST_TRIVIAL_HANDLE_TYPES(HANDLE_UNREGISTER_IMPL_IMPL)

    void unregister_VkInstance(VkInstance instance) {
        AutoLock lock(mLock);

        auto it = info_VkInstance.find(instance);
        if (it == info_VkInstance.end()) return;
        auto info = it->second;
        info_VkInstance.erase(instance);
        lock.unlock();
    }

    void unregister_VkDevice(VkDevice device) {
        AutoLock lock(mLock);

        auto it = info_VkDevice.find(device);
        if (it == info_VkDevice.end()) return;
        auto info = it->second;
        info_VkDevice.erase(device);
        lock.unlock();
    }

    void unregister_VkDeviceMemory(VkDeviceMemory mem) {
        AutoLock lock(mLock);

        auto it = info_VkDeviceMemory.find(mem);
        if (it == info_VkDeviceMemory.end()) return;

        auto& memInfo = it->second;

        if (memInfo.ahw) {
            AHardwareBuffer_release(memInfo.ahw);
        }

        if (memInfo.vmoHandle) {
            zx_handle_close(memInfo.vmoHandle);
        }

        if (memInfo.mappedPtr &&
            !memInfo.virtualHostVisibleBacking &&
            !memInfo.directMapped) {
            aligned_buf_free(memInfo.mappedPtr);
        }

        if (memInfo.directMapped) {
            subFreeHostMemory(&memInfo.subAlloc);
        }

        delete memInfo.goldfishAddressSpaceBlock;

        info_VkDeviceMemory.erase(mem);
    }

    void unregister_VkImage(VkImage img) {
        AutoLock lock(mLock);

        auto it = info_VkImage.find(img);
        if (it == info_VkImage.end()) return;

        info_VkImage.erase(img);
    }

    void unregister_VkBuffer(VkBuffer buf) {
        AutoLock lock(mLock);

        auto it = info_VkBuffer.find(buf);
        if (it == info_VkBuffer.end()) return;

        info_VkBuffer.erase(buf);
    }

    // TODO: Upgrade to 1.1
    static constexpr uint32_t kMaxApiVersion = VK_MAKE_VERSION(1, 0, 65);
    static constexpr uint32_t kMinApiVersion = VK_MAKE_VERSION(1, 0, 0);

    void setInstanceInfo(VkInstance instance,
                         uint32_t enabledExtensionCount,
                         const char* const* ppEnabledExtensionNames) {
        AutoLock lock(mLock);
        auto& info = info_VkInstance[instance];
        info.highestApiVersion = kMaxApiVersion;

        if (!ppEnabledExtensionNames) return;

        for (uint32_t i = 0; i < enabledExtensionCount; ++i) {
            info.enabledExtensions.insert(ppEnabledExtensionNames[i]);
        }
    }

    void setDeviceInfo(VkDevice device,
                       VkPhysicalDevice physdev,
                       VkPhysicalDeviceProperties props,
                       VkPhysicalDeviceMemoryProperties memProps,
                       uint32_t enabledExtensionCount,
                       const char* const* ppEnabledExtensionNames) {
        AutoLock lock(mLock);
        auto& info = info_VkDevice[device];
        info.physdev = physdev;
        info.props = props;
        info.memProps = memProps;
        initHostVisibleMemoryVirtualizationInfo(
            physdev, &memProps,
            mFeatureInfo->hasDirectMem,
            &mHostVisibleMemoryVirtInfo);
        info.apiVersion = props.apiVersion;

        if (!ppEnabledExtensionNames) return;

        for (uint32_t i = 0; i < enabledExtensionCount; ++i) {
            info.enabledExtensions.insert(ppEnabledExtensionNames[i]);
        }
    }

    void setDeviceMemoryInfo(VkDevice device,
                             VkDeviceMemory memory,
                             VkDeviceSize allocationSize,
                             VkDeviceSize mappedSize,
                             uint8_t* ptr,
                             uint32_t memoryTypeIndex,
                             AHardwareBuffer* ahw = nullptr,
                             zx_handle_t vmoHandle = ZX_HANDLE_INVALID) {
        AutoLock lock(mLock);
        auto& deviceInfo = info_VkDevice[device];
        auto& info = info_VkDeviceMemory[memory];

        info.allocationSize = allocationSize;
        info.mappedSize = mappedSize;
        info.mappedPtr = ptr;
        info.memoryTypeIndex = memoryTypeIndex;
        info.ahw = ahw;
        info.vmoHandle = vmoHandle;
    }

    bool isMemoryTypeHostVisible(VkDevice device, uint32_t typeIndex) const {
        AutoLock lock(mLock);
        const auto it = info_VkDevice.find(device);

        if (it == info_VkDevice.end()) return false;

        const auto& info = it->second;
        return info.memProps.memoryTypes[typeIndex].propertyFlags &
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    }

    uint8_t* getMappedPointer(VkDeviceMemory memory) {
        AutoLock lock(mLock);
        const auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) return nullptr;

        const auto& info = it->second;
        return info.mappedPtr;
    }

    VkDeviceSize getMappedSize(VkDeviceMemory memory) {
        AutoLock lock(mLock);
        const auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) return 0;

        const auto& info = it->second;
        return info.mappedSize;
    }

    VkDeviceSize getNonCoherentExtendedSize(VkDevice device, VkDeviceSize basicSize) const {
        AutoLock lock(mLock);
        const auto it = info_VkDevice.find(device);
        if (it == info_VkDevice.end()) return basicSize;
        const auto& info = it->second;

        VkDeviceSize nonCoherentAtomSize =
            info.props.limits.nonCoherentAtomSize;
        VkDeviceSize atoms =
            (basicSize + nonCoherentAtomSize - 1) / nonCoherentAtomSize;
        return atoms * nonCoherentAtomSize;
    }

    bool isValidMemoryRange(const VkMappedMemoryRange& range) const {
        AutoLock lock(mLock);
        const auto it = info_VkDeviceMemory.find(range.memory);
        if (it == info_VkDeviceMemory.end()) return false;
        const auto& info = it->second;

        if (!info.mappedPtr) return false;

        VkDeviceSize offset = range.offset;
        VkDeviceSize size = range.size;

        if (size == VK_WHOLE_SIZE) {
            return offset <= info.mappedSize;
        }

        return offset + size <= info.mappedSize;
    }

    void setupFeatures(const EmulatorFeatureInfo* features) {
        if (!features || mFeatureInfo) return;
        mFeatureInfo.reset(new EmulatorFeatureInfo);
        *mFeatureInfo = *features;

        if (mFeatureInfo->hasDirectMem) {
            mGoldfishAddressSpaceBlockProvider.reset(
                new GoldfishAddressSpaceBlockProvider);
        }
    }

    bool hostSupportsVulkan() const {
        if (!mFeatureInfo) return false;

        return mFeatureInfo->hasVulkan;
    }

    bool usingDirectMapping() const {
        return mHostVisibleMemoryVirtInfo.virtualizationSupported;
    }

    int getHostInstanceExtensionIndex(const std::string& extName) const {
        int i = 0;
        for (const auto& prop : mHostInstanceExtensions) {
            if (extName == std::string(prop.extensionName)) {
                return i;
            }
            ++i;
        }
        return -1;
    }

    int getHostDeviceExtensionIndex(const std::string& extName) const {
        int i = 0;
        for (const auto& prop : mHostDeviceExtensions) {
            if (extName == std::string(prop.extensionName)) {
                return i;
            }
            ++i;
        }
        return -1;
    }

    void deviceMemoryTransform_tohost(
        VkDeviceMemory* memory, uint32_t memoryCount,
        VkDeviceSize* offset, uint32_t offsetCount,
        VkDeviceSize* size, uint32_t sizeCount,
        uint32_t* typeIndex, uint32_t typeIndexCount,
        uint32_t* typeBits, uint32_t typeBitsCount) {

        (void)memoryCount;
        (void)offsetCount;
        (void)sizeCount;

        const auto& hostVirt =
            mHostVisibleMemoryVirtInfo;

        if (!hostVirt.virtualizationSupported) return;

        if (memory) {
            AutoLock lock (mLock);

            for (uint32_t i = 0; i < memoryCount; ++i) {
                VkDeviceMemory mem = memory[i];

                auto it = info_VkDeviceMemory.find(mem);
                if (it == info_VkDeviceMemory.end()) return;

                const auto& info = it->second;

                if (!info.directMapped) continue;

                memory[i] = info.subAlloc.baseMemory;

                if (offset) {
                    offset[i] = info.subAlloc.baseOffset + offset[i];
                }

                if (size) {
                    if (size[i] == VK_WHOLE_SIZE) {
                        size[i] = info.subAlloc.subMappedSize;
                    }
                }

                // TODO
                (void)memory;
                (void)offset;
                (void)size;
            }
        }

        for (uint32_t i = 0; i < typeIndexCount; ++i) {
            typeIndex[i] =
                hostVirt.memoryTypeIndexMappingToHost[typeIndex[i]];
        }

        for (uint32_t i = 0; i < typeBitsCount; ++i) {
            uint32_t bits = 0;
            for (uint32_t j = 0; j < VK_MAX_MEMORY_TYPES; ++j) {
                bool guestHas = typeBits[i] & (1 << j);
                uint32_t hostIndex =
                    hostVirt.memoryTypeIndexMappingToHost[j];
                bits |= guestHas ? (1 << hostIndex) : 0;
            }
            typeBits[i] = bits;
        }
    }

    void deviceMemoryTransform_fromhost(
        VkDeviceMemory* memory, uint32_t memoryCount,
        VkDeviceSize* offset, uint32_t offsetCount,
        VkDeviceSize* size, uint32_t sizeCount,
        uint32_t* typeIndex, uint32_t typeIndexCount,
        uint32_t* typeBits, uint32_t typeBitsCount) {

        (void)memoryCount;
        (void)offsetCount;
        (void)sizeCount;

        const auto& hostVirt =
            mHostVisibleMemoryVirtInfo;

        if (!hostVirt.virtualizationSupported) return;

        AutoLock lock (mLock);

        for (uint32_t i = 0; i < memoryCount; ++i) {
            // TODO
            (void)memory;
            (void)offset;
            (void)size;
        }

        for (uint32_t i = 0; i < typeIndexCount; ++i) {
            typeIndex[i] =
                hostVirt.memoryTypeIndexMappingFromHost[typeIndex[i]];
        }

        for (uint32_t i = 0; i < typeBitsCount; ++i) {
            uint32_t bits = 0;
            for (uint32_t j = 0; j < VK_MAX_MEMORY_TYPES; ++j) {
                bool hostHas = typeBits[i] & (1 << j);
                uint32_t guestIndex =
                    hostVirt.memoryTypeIndexMappingFromHost[j];
                bits |= hostHas ? (1 << guestIndex) : 0;

                if (hostVirt.memoryTypeBitsShouldAdvertiseBoth[j]) {
                    bits |= hostHas ? (1 << j) : 0;
                }
            }
            typeBits[i] = bits;
        }
    }

    VkResult on_vkEnumerateInstanceVersion(
        void*,
        VkResult,
        uint32_t* apiVersion) {
        if (apiVersion) {
            *apiVersion = kMaxApiVersion;
        }
        return VK_SUCCESS;
    }

    VkResult on_vkEnumerateInstanceExtensionProperties(
        void* context,
        VkResult,
        const char*,
        uint32_t* pPropertyCount,
        VkExtensionProperties* pProperties) {

        std::vector<const char*> allowedExtensionNames = {
            "VK_KHR_get_physical_device_properties2",
            "VK_KHR_sampler_ycbcr_conversion",
            // TODO:
            // VK_KHR_external_memory_capabilities
        };

        VkEncoder* enc = (VkEncoder*)context;

        // Only advertise a select set of extensions.
        if (mHostInstanceExtensions.empty()) {
            uint32_t hostPropCount = 0;
            enc->vkEnumerateInstanceExtensionProperties(nullptr, &hostPropCount, nullptr);
            mHostInstanceExtensions.resize(hostPropCount);

            VkResult hostRes =
                enc->vkEnumerateInstanceExtensionProperties(
                    nullptr, &hostPropCount, mHostInstanceExtensions.data());

            if (hostRes != VK_SUCCESS) {
                return hostRes;
            }
        }

        std::vector<VkExtensionProperties> filteredExts;

        for (size_t i = 0; i < allowedExtensionNames.size(); ++i) {
            auto extIndex = getHostInstanceExtensionIndex(allowedExtensionNames[i]);
            if (extIndex != -1) {
                filteredExts.push_back(mHostInstanceExtensions[extIndex]);
            }
        }

        VkExtensionProperties anbExtProp = {
            "VK_ANDROID_native_buffer", 7,
        };

        filteredExts.push_back(anbExtProp);

        if (pPropertyCount) {
            *pPropertyCount = filteredExts.size();
        }

        if (pPropertyCount && pProperties) {
            for (size_t i = 0; i < *pPropertyCount; ++i) {
                pProperties[i] = filteredExts[i];
            }
        }

        return VK_SUCCESS;
    }

    VkResult on_vkEnumerateDeviceExtensionProperties(
        void* context,
        VkResult,
        VkPhysicalDevice physdev,
        const char*,
        uint32_t* pPropertyCount,
        VkExtensionProperties* pProperties) {

        std::vector<const char*> allowedExtensionNames = {
            "VK_KHR_maintenance1",
            "VK_KHR_get_memory_requirements2",
            "VK_KHR_dedicated_allocation",
            "VK_KHR_bind_memory2",
            "VK_KHR_sampler_ycbcr_conversion",
            // "VK_KHR_maintenance2",
            // "VK_KHR_maintenance3",
            // TODO:
            // VK_KHR_external_memory_capabilities
        };

        VkEncoder* enc = (VkEncoder*)context;

        if (mHostDeviceExtensions.empty()) {
            uint32_t hostPropCount = 0;
            enc->vkEnumerateDeviceExtensionProperties(physdev, nullptr, &hostPropCount, nullptr);
            mHostDeviceExtensions.resize(hostPropCount);

            VkResult hostRes =
                enc->vkEnumerateDeviceExtensionProperties(
                    physdev, nullptr, &hostPropCount, mHostDeviceExtensions.data());

            if (hostRes != VK_SUCCESS) {
                return hostRes;
            }
        }

        std::vector<VkExtensionProperties> filteredExts;

        for (size_t i = 0; i < allowedExtensionNames.size(); ++i) {
            auto extIndex = getHostDeviceExtensionIndex(allowedExtensionNames[i]);
            if (extIndex != -1) {
                filteredExts.push_back(mHostDeviceExtensions[extIndex]);
            }
        }

        VkExtensionProperties anbExtProp = {
            "VK_ANDROID_native_buffer", 7,
        };

        filteredExts.push_back(anbExtProp);

        if (pPropertyCount) {
            *pPropertyCount = filteredExts.size();
        }

        if (pPropertyCount && pProperties) {
            for (size_t i = 0; i < *pPropertyCount; ++i) {
                pProperties[i] = filteredExts[i];
            }
        }

        return VK_SUCCESS;
    }

    VkResult on_vkEnumeratePhysicalDevices(
        void* context, VkResult,
        VkInstance instance, uint32_t* pPhysicalDeviceCount,
        VkPhysicalDevice* pPhysicalDevices) {

        VkEncoder* enc = (VkEncoder*)context;

        if (!instance) return VK_ERROR_INITIALIZATION_FAILED;

        if (!pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;

        AutoLock lock(mLock);

        auto it = info_VkInstance.find(instance);

        if (it == info_VkInstance.end()) return VK_ERROR_INITIALIZATION_FAILED;

        auto& info = it->second;

        if (info.physicalDevices.empty()) {
            uint32_t physdevCount = 0;

            lock.unlock();
            VkResult countRes = enc->vkEnumeratePhysicalDevices(
                instance, &physdevCount, nullptr);
            lock.lock();

            if (countRes != VK_SUCCESS) {
                ALOGE("%s: failed: could not count host physical devices. "
                      "Error %d\n", __func__, countRes);
                return countRes;
            }

            info.physicalDevices.resize(physdevCount);

            lock.unlock();
            VkResult enumRes = enc->vkEnumeratePhysicalDevices(
                instance, &physdevCount, info.physicalDevices.data());
            lock.lock();

            if (enumRes != VK_SUCCESS) {
                ALOGE("%s: failed: could not retrieve host physical devices. "
                      "Error %d\n", __func__, enumRes);
                return enumRes;
            }
        }

        *pPhysicalDeviceCount = (uint32_t)info.physicalDevices.size();

        if (pPhysicalDevices && *pPhysicalDeviceCount) {
            memcpy(pPhysicalDevices,
                   info.physicalDevices.data(),
                   sizeof(VkPhysicalDevice) *
                   info.physicalDevices.size());
        }

        return VK_SUCCESS;
    }

    void on_vkGetPhysicalDeviceMemoryProperties(
        void*,
        VkPhysicalDevice physdev,
        VkPhysicalDeviceMemoryProperties* out) {

        initHostVisibleMemoryVirtualizationInfo(
            physdev,
            out,
            mFeatureInfo->hasDirectMem,
            &mHostVisibleMemoryVirtInfo);

        if (mHostVisibleMemoryVirtInfo.virtualizationSupported) {
            *out = mHostVisibleMemoryVirtInfo.guestMemoryProperties;
        }
    }

    void on_vkGetPhysicalDeviceMemoryProperties2(
        void*,
        VkPhysicalDevice physdev,
        VkPhysicalDeviceMemoryProperties2* out) {

        initHostVisibleMemoryVirtualizationInfo(
            physdev,
            &out->memoryProperties,
            mFeatureInfo->hasDirectMem,
            &mHostVisibleMemoryVirtInfo);

        if (mHostVisibleMemoryVirtInfo.virtualizationSupported) {
            out->memoryProperties = mHostVisibleMemoryVirtInfo.guestMemoryProperties;
        }
    }

    VkResult on_vkCreateInstance(
        void*,
        VkResult input_result,
        const VkInstanceCreateInfo* createInfo,
        const VkAllocationCallbacks*,
        VkInstance* pInstance) {

        if (input_result != VK_SUCCESS) return input_result;

        setInstanceInfo(
            *pInstance,
            createInfo->enabledExtensionCount,
            createInfo->ppEnabledExtensionNames);

        return input_result;
    }

    VkResult on_vkCreateDevice(
        void* context,
        VkResult input_result,
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks*,
        VkDevice* pDevice) {

        if (input_result != VK_SUCCESS) return input_result;

        VkEncoder* enc = (VkEncoder*)context;

        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceMemoryProperties memProps;
        enc->vkGetPhysicalDeviceProperties(physicalDevice, &props);
        enc->vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

        setDeviceInfo(
            *pDevice, physicalDevice, props, memProps,
            pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames);

        return input_result;
    }

    void on_vkDestroyDevice_pre(
        void* context,
        VkDevice device,
        const VkAllocationCallbacks*) {

        AutoLock lock(mLock);

        auto it = info_VkDevice.find(device);
        if (it == info_VkDevice.end()) return;
        auto info = it->second;

        lock.unlock();

        VkEncoder* enc = (VkEncoder*)context;

        for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; ++i) {
            destroyHostMemAlloc(enc, device, &info.hostMemAllocs[i]);
        }
    }

    VkResult on_vkGetAndroidHardwareBufferPropertiesANDROID(
        VkDevice device,
        const AHardwareBuffer* buffer,
        VkAndroidHardwareBufferPropertiesANDROID* pProperties) {
        return getAndroidHardwareBufferPropertiesANDROID(
            &mHostVisibleMemoryVirtInfo,
            device, buffer, pProperties);
    }

    VkResult on_vkGetMemoryAndroidHardwareBufferANDROID(
        VkDevice device,
        const VkMemoryGetAndroidHardwareBufferInfoANDROID *pInfo,
        struct AHardwareBuffer** pBuffer) {

        if (!pInfo) return VK_ERROR_INITIALIZATION_FAILED;
        if (!pInfo->memory) return VK_ERROR_INITIALIZATION_FAILED;

        AutoLock lock(mLock);

        auto deviceIt = info_VkDevice.find(device);

        if (deviceIt == info_VkDevice.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto memoryIt = info_VkDeviceMemory.find(pInfo->memory);

        if (memoryIt == info_VkDeviceMemory.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto& info = memoryIt->second;

        VkResult queryRes =
            getMemoryAndroidHardwareBufferANDROID(&info.ahw);

        if (queryRes != VK_SUCCESS) return queryRes;

        *pBuffer = info.ahw;

        return queryRes;
    }

    VkResult on_vkAllocateMemory(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkMemoryAllocateInfo* pAllocateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDeviceMemory* pMemory) {

        if (input_result != VK_SUCCESS) return input_result;

        VkEncoder* enc = (VkEncoder*)context;

        VkMemoryAllocateInfo finalAllocInfo = *pAllocateInfo;
        VkMemoryDedicatedAllocateInfo dedicatedAllocInfo;
        VkImportColorBufferGOOGLE importCbInfo = {
            VK_STRUCTURE_TYPE_IMPORT_COLOR_BUFFER_GOOGLE, 0,
        };
        VkImportPhysicalAddressGOOGLE importPhysAddrInfo = {
            VK_STRUCTURE_TYPE_IMPORT_PHYSICAL_ADDRESS_GOOGLE, 0,
        };

        vk_struct_common* structChain =
        structChain = vk_init_struct_chain(
            (vk_struct_common*)(&finalAllocInfo));
        structChain->pNext = nullptr;

        VkExportMemoryAllocateInfo* exportAllocateInfoPtr =
            (VkExportMemoryAllocateInfo*)vk_find_struct((vk_struct_common*)pAllocateInfo,
                VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);

        VkImportAndroidHardwareBufferInfoANDROID* importAhbInfoPtr =
            (VkImportAndroidHardwareBufferInfoANDROID*)vk_find_struct((vk_struct_common*)pAllocateInfo,
                VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID);

        // TODO: Fuchsia image works in a similar way but over vmo id (phys addr)?
        VkImportPhysicalAddressGOOGLE* importPhysAddrInfoPtr = nullptr;

        VkMemoryDedicatedAllocateInfo* dedicatedAllocInfoPtr =
            (VkMemoryDedicatedAllocateInfo*)vk_find_struct((vk_struct_common*)pAllocateInfo,
                VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);

        bool shouldPassThroughDedicatedAllocInfo =
            !isHostVisibleMemoryTypeIndexForGuest(
                &mHostVisibleMemoryVirtInfo,
                pAllocateInfo->memoryTypeIndex);

        if (!exportAllocateInfoPtr &&
            importAhbInfoPtr && // TODO: Fuchsia image
            dedicatedAllocInfoPtr &&
            isHostVisibleMemoryTypeIndexForGuest(
                &mHostVisibleMemoryVirtInfo,
                pAllocateInfo->memoryTypeIndex)) {
            ALOGE("FATAL: It is not yet supported to import-allocate "
                  "external memory that is both host visible and dedicated.");
            abort();
        }

        if (shouldPassThroughDedicatedAllocInfo &&
            dedicatedAllocInfoPtr) {
            dedicatedAllocInfo = *dedicatedAllocInfoPtr;
            structChain->pNext =
                (vk_struct_common*)(&dedicatedAllocInfo);
            structChain =
                (vk_struct_common*)(&dedicatedAllocInfo);
            structChain->pNext = nullptr;
        }

        // State needed for import/export.
        bool exportAhb = false;
        bool exportPhysAddr = false;
        bool importAhb = false;
        bool importPhysAddr = false;
        (void)exportPhysAddr;
        (void)importPhysAddr;

        // Even if we export allocate, the underlying operation
        // for the host is always going to be an import operation.
        // This is also how Intel's implementation works,
        // and is generally simpler;
        // even in an export allocation,
        // we perform AHardwareBuffer allocation
        // on the guest side, at this layer,
        // and then we attach a new VkDeviceMemory
        // to the AHardwareBuffer on the host via an "import" operation.
        AHardwareBuffer* ahw = nullptr;
        zx_handle_t vmo_handle = ZX_HANDLE_INVALID;

        if (exportAllocateInfoPtr) {
            exportAhb =
                exportAllocateInfoPtr->handleTypes &
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        } else if (importAhbInfoPtr) {
            importAhb = true;
        }

        if (importPhysAddrInfoPtr) {
            importPhysAddrInfo = *importPhysAddrInfoPtr;
            importPhysAddr = true;
        }

        if (exportAhb) {
            bool hasDedicatedImage = dedicatedAllocInfoPtr &&
                (dedicatedAllocInfoPtr->image != VK_NULL_HANDLE);
            bool hasDedicatedBuffer = dedicatedAllocInfoPtr &&
                (dedicatedAllocInfoPtr->buffer != VK_NULL_HANDLE);
            VkExtent3D imageExtent = { 0, 0, 0 };
            uint32_t imageLayers = 0;
            VkFormat imageFormat = VK_FORMAT_UNDEFINED;
            VkImageUsageFlags imageUsage = 0;
            VkImageCreateFlags imageCreateFlags = 0;
            VkDeviceSize bufferSize = 0;
            VkDeviceSize allocationInfoAllocSize =
                finalAllocInfo.allocationSize;

            if (hasDedicatedImage) {
                AutoLock lock(mLock);

                auto it = info_VkImage.find(
                    dedicatedAllocInfoPtr->image);
                if (it == info_VkImage.end()) return VK_ERROR_INITIALIZATION_FAILED;
                const auto& info = it->second;
                const auto& imgCi = info.createInfo;

                imageExtent = imgCi.extent;
                imageLayers = imgCi.arrayLayers;
                imageFormat = imgCi.format;
                imageUsage = imgCi.usage;
                imageCreateFlags = imgCi.flags;
            }

            if (hasDedicatedBuffer) {
                AutoLock lock(mLock);

                auto it = info_VkBuffer.find(
                    dedicatedAllocInfoPtr->buffer);
                if (it == info_VkBuffer.end()) return VK_ERROR_INITIALIZATION_FAILED;
                const auto& info = it->second;
                const auto& bufCi = info.createInfo;

                bufferSize = bufCi.size;
            }

            VkResult ahbCreateRes =
                createAndroidHardwareBuffer(
                    hasDedicatedImage,
                    hasDedicatedBuffer,
                    imageExtent,
                    imageLayers,
                    imageFormat,
                    imageUsage,
                    imageCreateFlags,
                    bufferSize,
                    allocationInfoAllocSize,
                    &ahw);

            if (ahbCreateRes != VK_SUCCESS) {
                return ahbCreateRes;
            }
        }

        if (importAhb) {
            ahw = importAhbInfoPtr->buffer;
            // We still need to acquire the AHardwareBuffer.
            importAndroidHardwareBuffer(
                importAhbInfoPtr, nullptr);
        }

        if (ahw) {
            const native_handle_t *handle =
                AHardwareBuffer_getNativeHandle(ahw);
            const cb_handle_t* cb_handle =
                reinterpret_cast<const cb_handle_t*>(handle);
            importCbInfo.colorBuffer = cb_handle->hostHandle;
            structChain =
                vk_append_struct(structChain, (vk_struct_common*)(&importCbInfo));
        }

        // TODO if (exportPhysAddr) { }

        if (!isHostVisibleMemoryTypeIndexForGuest(
                &mHostVisibleMemoryVirtInfo,
                finalAllocInfo.memoryTypeIndex)) {

            input_result =
                enc->vkAllocateMemory(
                    device, &finalAllocInfo, pAllocator, pMemory);

            if (input_result != VK_SUCCESS) return input_result;

            VkDeviceSize allocationSize = finalAllocInfo.allocationSize;
            setDeviceMemoryInfo(
                device, *pMemory,
                finalAllocInfo.allocationSize,
                0, nullptr,
                finalAllocInfo.memoryTypeIndex,
                ahw,
                vmo_handle);

            return VK_SUCCESS;
        }

        // Device-local memory dealing is over. What follows:
        // host-visible memory.

        if (ahw) {
            ALOGE("%s: Host visible export/import allocation "
                  "of Android hardware buffers is not supported.",
                  __func__);
            abort();
        }

        // Host visible memory, non external
        bool directMappingSupported = usingDirectMapping();
        if (!directMappingSupported) {
            input_result =
                enc->vkAllocateMemory(
                    device, &finalAllocInfo, pAllocator, pMemory);

            if (input_result != VK_SUCCESS) return input_result;

            VkDeviceSize mappedSize =
                getNonCoherentExtendedSize(device,
                    finalAllocInfo.allocationSize);
            uint8_t* mappedPtr = (uint8_t*)aligned_buf_alloc(4096, mappedSize);
            D("host visible alloc (non-direct): "
              "size 0x%llx host ptr %p mapped size 0x%llx",
              (unsigned long long)finalAllocInfo.allocationSize, mappedPtr,
              (unsigned long long)mappedSize);
            setDeviceMemoryInfo(
                device, *pMemory,
                finalAllocInfo.allocationSize,
                mappedSize, mappedPtr,
                finalAllocInfo.memoryTypeIndex);
            return VK_SUCCESS;
        }

        // Host visible memory with direct mapping via
        // VkImportPhysicalAddressGOOGLE
        if (importPhysAddr) {
            // vkAllocateMemory(device, &finalAllocInfo, pAllocator, pMemory);
            //    host maps the host pointer to the guest physical address
            // TODO: the host side page offset of the
            // host pointer needs to be returned somehow.
        }

        // Host visible memory with direct mapping
        AutoLock lock(mLock);

        auto it = info_VkDevice.find(device);
        if (it == info_VkDevice.end()) return VK_ERROR_DEVICE_LOST;
        auto& deviceInfo = it->second;

        HostMemAlloc* hostMemAlloc =
            &deviceInfo.hostMemAllocs[finalAllocInfo.memoryTypeIndex];

        if (!hostMemAlloc->initialized) {
            VkMemoryAllocateInfo allocInfoForHost = finalAllocInfo;
            allocInfoForHost.allocationSize = VIRTUAL_HOST_VISIBLE_HEAP_SIZE;
            // TODO: Support dedicated allocation
            allocInfoForHost.pNext = nullptr;

            lock.unlock();
            VkResult host_res =
                enc->vkAllocateMemory(
                    device,
                    &allocInfoForHost,
                    nullptr,
                    &hostMemAlloc->memory);
            lock.lock();

            if (host_res != VK_SUCCESS) {
                ALOGE("Could not allocate backing for virtual host visible memory: %d",
                      host_res);
                hostMemAlloc->initialized = true;
                hostMemAlloc->initResult = host_res;
                return host_res;
            }

            auto& hostMemInfo = info_VkDeviceMemory[hostMemAlloc->memory];
            hostMemInfo.allocationSize = allocInfoForHost.allocationSize;
            VkDeviceSize nonCoherentAtomSize =
                deviceInfo.props.limits.nonCoherentAtomSize;
            hostMemInfo.mappedSize = hostMemInfo.allocationSize;
            hostMemInfo.memoryTypeIndex =
                finalAllocInfo.memoryTypeIndex;
            hostMemAlloc->nonCoherentAtomSize = nonCoherentAtomSize;

            uint64_t directMappedAddr = 0;
            lock.unlock();
            VkResult directMapResult =
                enc->vkMapMemoryIntoAddressSpaceGOOGLE(
                    device, hostMemAlloc->memory, &directMappedAddr);
            lock.lock();

            if (directMapResult != VK_SUCCESS) {
                hostMemAlloc->initialized = true;
                hostMemAlloc->initResult = directMapResult;
                return directMapResult;
            }

            hostMemInfo.mappedPtr =
                (uint8_t*)(uintptr_t)directMappedAddr;
            hostMemInfo.virtualHostVisibleBacking = true;

            VkResult hostMemAllocRes =
                finishHostMemAllocInit(
                    enc,
                    device,
                    finalAllocInfo.memoryTypeIndex,
                    nonCoherentAtomSize,
                    hostMemInfo.allocationSize,
                    hostMemInfo.mappedSize,
                    hostMemInfo.mappedPtr,
                    hostMemAlloc);

            if (hostMemAllocRes != VK_SUCCESS) {
                return hostMemAllocRes;
            }
        }

        VkDeviceMemory_Info virtualMemInfo;

        subAllocHostMemory(
            hostMemAlloc,
            &finalAllocInfo,
            &virtualMemInfo.subAlloc);

        virtualMemInfo.allocationSize = virtualMemInfo.subAlloc.subAllocSize;
        virtualMemInfo.mappedSize = virtualMemInfo.subAlloc.subMappedSize;
        virtualMemInfo.mappedPtr = virtualMemInfo.subAlloc.mappedPtr;
        virtualMemInfo.memoryTypeIndex = finalAllocInfo.memoryTypeIndex;
        virtualMemInfo.directMapped = true;

        D("host visible alloc (direct, suballoc): "
          "size 0x%llx ptr %p mapped size 0x%llx",
          (unsigned long long)virtualMemInfo.allocationSize, virtualMemInfo.mappedPtr,
          (unsigned long long)virtualMemInfo.mappedSize);

        info_VkDeviceMemory[
            virtualMemInfo.subAlloc.subMemory] = virtualMemInfo;

        *pMemory = virtualMemInfo.subAlloc.subMemory;

        return VK_SUCCESS;
    }

    void on_vkFreeMemory(
        void* context,
        VkDevice device,
        VkDeviceMemory memory,
        const VkAllocationCallbacks* pAllocateInfo) {

        AutoLock lock(mLock);

        auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) return;
        auto& info = it->second;

        if (!info.directMapped) {
            lock.unlock();
            VkEncoder* enc = (VkEncoder*)context;
            enc->vkFreeMemory(device, memory, pAllocateInfo);
            return;
        }

        subFreeHostMemory(&info.subAlloc);
    }

    VkResult on_vkMapMemory(
        void*,
        VkResult host_result,
        VkDevice,
        VkDeviceMemory memory,
        VkDeviceSize offset,
        VkDeviceSize size,
        VkMemoryMapFlags,
        void** ppData) {

        if (host_result != VK_SUCCESS) return host_result;

        AutoLock lock(mLock);

        auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) return VK_ERROR_MEMORY_MAP_FAILED;

        auto& info = it->second;

        if (!info.mappedPtr) return VK_ERROR_MEMORY_MAP_FAILED;

        if (size != VK_WHOLE_SIZE &&
            (info.mappedPtr + offset + size > info.mappedPtr + info.allocationSize)) {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        *ppData = info.mappedPtr + offset;

        return host_result;
    }

    void on_vkUnmapMemory(
        void*,
        VkDevice,
        VkDeviceMemory) {
        // no-op
    }

    VkResult on_vkCreateImage(
        void* context, VkResult,
        VkDevice device, const VkImageCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkImage *pImage) {
        VkEncoder* enc = (VkEncoder*)context;

        VkResult res = enc->vkCreateImage(device, pCreateInfo, pAllocator, pImage);

        if (res != VK_SUCCESS) return res;

        AutoLock lock(mLock);

        auto it = info_VkImage.find(*pImage);
        if (it == info_VkImage.end()) return VK_ERROR_INITIALIZATION_FAILED;

        auto& info = it->second;

        info.createInfo = *pCreateInfo;
        info.createInfo.pNext = nullptr;

        return res;
    }

    void on_vkDestroyImage(
        void* context,
        VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkDestroyImage(device, image, pAllocator);
    }

    void on_vkGetImageMemoryRequirements(
        void *context, VkDevice device, VkImage image,
        VkMemoryRequirements *pMemoryRequirements) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetImageMemoryRequirements(
            device, image, pMemoryRequirements);
    }

    void on_vkGetImageMemoryRequirements2(
        void *context, VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
        VkMemoryRequirements2 *pMemoryRequirements) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetImageMemoryRequirements2(
            device, pInfo, pMemoryRequirements);
    }

    void on_vkGetImageMemoryRequirements2KHR(
        void *context, VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
        VkMemoryRequirements2 *pMemoryRequirements) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetImageMemoryRequirements2KHR(
            device, pInfo, pMemoryRequirements);
    }

    VkResult on_vkBindImageMemory(
        void* context, VkResult,
        VkDevice device, VkImage image, VkDeviceMemory memory,
        VkDeviceSize memoryOffset) {
        VkEncoder* enc = (VkEncoder*)context;
        return enc->vkBindImageMemory(device, image, memory, memoryOffset);
    }

    VkResult on_vkBindImageMemory2(
        void* context, VkResult,
        VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos) {
        VkEncoder* enc = (VkEncoder*)context;
        return enc->vkBindImageMemory2(device, bindingCount, pBindInfos);
    }

    VkResult on_vkBindImageMemory2KHR(
        void* context, VkResult,
        VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos) {
        VkEncoder* enc = (VkEncoder*)context;
        return enc->vkBindImageMemory2KHR(device, bindingCount, pBindInfos);
    }

    VkResult on_vkCreateBuffer(
        void* context, VkResult,
        VkDevice device, const VkBufferCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkBuffer *pBuffer) {
        VkEncoder* enc = (VkEncoder*)context;

        VkResult res = enc->vkCreateBuffer(device, pCreateInfo, pAllocator, pBuffer);

        if (res != VK_SUCCESS) return res;

        AutoLock lock(mLock);

        auto it = info_VkBuffer.find(*pBuffer);
        if (it == info_VkBuffer.end()) return VK_ERROR_INITIALIZATION_FAILED;

        auto& info = it->second;

        info.createInfo = *pCreateInfo;
        info.createInfo.pNext = nullptr;

        return res;
    }

    void on_vkDestroyBuffer(
        void* context,
        VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkDestroyBuffer(device, buffer, pAllocator);
    }

    void on_vkGetBufferMemoryRequirements(
        void* context, VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetBufferMemoryRequirements(
            device, buffer, pMemoryRequirements);
    }

    void on_vkGetBufferMemoryRequirements2(
        void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetBufferMemoryRequirements2(device, pInfo, pMemoryRequirements);
    }

    void on_vkGetBufferMemoryRequirements2KHR(
        void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetBufferMemoryRequirements2KHR(device, pInfo, pMemoryRequirements);
    }

    VkResult on_vkBindBufferMemory(
        void *context, VkResult,
        VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
        VkEncoder *enc = (VkEncoder *)context;
        return enc->vkBindBufferMemory(
            device, buffer, memory, memoryOffset);
    }

    VkResult on_vkBindBufferMemory2(
        void *context, VkResult,
        VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
        VkEncoder *enc = (VkEncoder *)context;
        return enc->vkBindBufferMemory2(
            device, bindInfoCount, pBindInfos);
    }

    VkResult on_vkBindBufferMemory2KHR(
        void *context, VkResult,
        VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
        VkEncoder *enc = (VkEncoder *)context;
        return enc->vkBindBufferMemory2KHR(
            device, bindInfoCount, pBindInfos);
    }

    VkResult on_vkCreateSemaphore(
        void* context, VkResult,
        VkDevice device, const VkSemaphoreCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSemaphore* pSemaphore) {
        VkEncoder* enc = (VkEncoder*)context;
        return enc->vkCreateSemaphore(
            device, pCreateInfo, pAllocator, pSemaphore);
    }

    void on_vkDestroySemaphore(
        void* context,
        VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkDestroySemaphore(device, semaphore, pAllocator);
    }

    VkResult on_vkQueueSubmit(
        void* context, VkResult,
        VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
        VkEncoder* enc = (VkEncoder*)context;
        return enc->vkQueueSubmit(queue, submitCount, pSubmits, fence);
    }

    void unwrap_VkNativeBufferANDROID(
        const VkImageCreateInfo* pCreateInfo,
        VkImageCreateInfo* local_pCreateInfo) {

        if (!pCreateInfo->pNext) return;

        const VkNativeBufferANDROID* nativeInfo =
            reinterpret_cast<const VkNativeBufferANDROID*>(pCreateInfo->pNext);

        if (VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID != nativeInfo->sType) {
            return;
        }

        const cb_handle_t* cb_handle =
            reinterpret_cast<const cb_handle_t*>(nativeInfo->handle);

        if (!cb_handle) return;

        VkNativeBufferANDROID* nativeInfoOut =
            reinterpret_cast<VkNativeBufferANDROID*>(
                const_cast<void*>(
                    local_pCreateInfo->pNext));

        if (!nativeInfoOut->handle) {
            ALOGE("FATAL: Local native buffer info not properly allocated!");
            abort();
        }

        *(uint32_t*)(nativeInfoOut->handle) = cb_handle->hostHandle;
    }

    void unwrap_vkAcquireImageANDROID_nativeFenceFd(int fd, int*) {
        if (fd != -1) {
            sync_wait(fd, 3000);
        }
    }

    // Action of vkMapMemoryIntoAddressSpaceGOOGLE:
    // 1. preprocess (on_vkMapMemoryIntoAddressSpaceGOOGLE_pre):
    //    uses address space device to reserve the right size of
    //    memory.
    // 2. the reservation results in a physical address. the physical
    //    address is set as |*pAddress|.
    // 3. after pre, the API call is encoded to the host, where the
    //    value of pAddress is also sent (the physical address).
    // 4. the host will obtain the actual gpu pointer and send it
    //    back out in |*pAddress|.
    // 5. postprocess (on_vkMapMemoryIntoAddressSpaceGOOGLE) will run,
    //    using the mmap() method of GoldfishAddressSpaceBlock to obtain
    //    a pointer in guest userspace corresponding to the host pointer.
    VkResult on_vkMapMemoryIntoAddressSpaceGOOGLE_pre(
        void*,
        VkResult,
        VkDevice,
        VkDeviceMemory memory,
        uint64_t* pAddress) {

        AutoLock lock(mLock);

        auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        auto& memInfo = it->second;
        memInfo.goldfishAddressSpaceBlock =
            new GoldfishAddressSpaceBlock;
        auto& block = *(memInfo.goldfishAddressSpaceBlock);

        block.allocate(
            mGoldfishAddressSpaceBlockProvider.get(),
            memInfo.mappedSize);

        *pAddress = block.physAddr();

        return VK_SUCCESS;
    }

    VkResult on_vkMapMemoryIntoAddressSpaceGOOGLE(
        void*,
        VkResult input_result,
        VkDevice,
        VkDeviceMemory memory,
        uint64_t* pAddress) {

        if (input_result != VK_SUCCESS) {
            return input_result;
        }

        // Now pAddress points to the gpu addr from host.
        AutoLock lock(mLock);

        auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        auto& memInfo = it->second;
        auto& block = *(memInfo.goldfishAddressSpaceBlock);

        uint64_t gpuAddr = *pAddress;

        void* userPtr = block.mmap(gpuAddr);

        D("%s: Got new host visible alloc. "
          "Sizeof void: %zu map size: %zu Range: [%p %p]",
          __func__,
          sizeof(void*), (size_t)memInfo.mappedSize,
          userPtr,
          (unsigned char*)userPtr + memInfo.mappedSize);

        *pAddress = (uint64_t)(uintptr_t)userPtr;

        return input_result;
    }

    uint32_t getApiVersionFromInstance(VkInstance instance) const {
        AutoLock lock(mLock);
        uint32_t api = kMinApiVersion;

        auto it = info_VkInstance.find(instance);
        if (it == info_VkInstance.end()) return api;

        api = it->second.highestApiVersion;

        return api;
    }

    uint32_t getApiVersionFromDevice(VkDevice device) const {
        AutoLock lock(mLock);

        uint32_t api = kMinApiVersion;

        auto it = info_VkDevice.find(device);
        if (it == info_VkDevice.end()) return api;

        api = it->second.apiVersion;

        return api;
    }

    bool hasInstanceExtension(VkInstance instance, const std::string& name) const {
        AutoLock lock(mLock);

        auto it = info_VkInstance.find(instance);
        if (it == info_VkInstance.end()) return false;

        return it->second.enabledExtensions.find(name) !=
               it->second.enabledExtensions.end();
    }

    bool hasDeviceExtension(VkDevice device, const std::string& name) const {
        AutoLock lock(mLock);

        auto it = info_VkDevice.find(device);
        if (it == info_VkDevice.end()) return false;

        return it->second.enabledExtensions.find(name) !=
               it->second.enabledExtensions.end();
    }

private:
    mutable Lock mLock;
    HostVisibleMemoryVirtualizationInfo mHostVisibleMemoryVirtInfo;
    std::unique_ptr<EmulatorFeatureInfo> mFeatureInfo;
    std::unique_ptr<GoldfishAddressSpaceBlockProvider> mGoldfishAddressSpaceBlockProvider;

    std::vector<VkExtensionProperties> mHostInstanceExtensions;
    std::vector<VkExtensionProperties> mHostDeviceExtensions;
};

ResourceTracker::ResourceTracker() : mImpl(new ResourceTracker::Impl()) { }
ResourceTracker::~ResourceTracker() { }
VulkanHandleMapping* ResourceTracker::createMapping() {
    return &mImpl->createMapping;
}
VulkanHandleMapping* ResourceTracker::unwrapMapping() {
    return &mImpl->unwrapMapping;
}
VulkanHandleMapping* ResourceTracker::destroyMapping() {
    return &mImpl->destroyMapping;
}
VulkanHandleMapping* ResourceTracker::defaultMapping() {
    return &mImpl->defaultMapping;
}
static ResourceTracker* sTracker = nullptr;
// static
ResourceTracker* ResourceTracker::get() {
    if (!sTracker) {
        // To be initialized once on vulkan device open.
        sTracker = new ResourceTracker;
    }
    return sTracker;
}

#define HANDLE_REGISTER_IMPL(type) \
    void ResourceTracker::register_##type(type obj) { \
        mImpl->register_##type(obj); \
    } \
    void ResourceTracker::unregister_##type(type obj) { \
        mImpl->unregister_##type(obj); \
    } \

GOLDFISH_VK_LIST_HANDLE_TYPES(HANDLE_REGISTER_IMPL)

bool ResourceTracker::isMemoryTypeHostVisible(
    VkDevice device, uint32_t typeIndex) const {
    return mImpl->isMemoryTypeHostVisible(device, typeIndex);
}

uint8_t* ResourceTracker::getMappedPointer(VkDeviceMemory memory) {
    return mImpl->getMappedPointer(memory);
}

VkDeviceSize ResourceTracker::getMappedSize(VkDeviceMemory memory) {
    return mImpl->getMappedSize(memory);
}

VkDeviceSize ResourceTracker::getNonCoherentExtendedSize(VkDevice device, VkDeviceSize basicSize) const {
    return mImpl->getNonCoherentExtendedSize(device, basicSize);
}

bool ResourceTracker::isValidMemoryRange(const VkMappedMemoryRange& range) const {
    return mImpl->isValidMemoryRange(range);
}

void ResourceTracker::setupFeatures(const EmulatorFeatureInfo* features) {
    mImpl->setupFeatures(features);
}

bool ResourceTracker::hostSupportsVulkan() const {
    return mImpl->hostSupportsVulkan();
}

bool ResourceTracker::usingDirectMapping() const {
    return mImpl->usingDirectMapping();
}

uint32_t ResourceTracker::getApiVersionFromInstance(VkInstance instance) const {
    return mImpl->getApiVersionFromInstance(instance);
}

uint32_t ResourceTracker::getApiVersionFromDevice(VkDevice device) const {
    return mImpl->getApiVersionFromDevice(device);
}
bool ResourceTracker::hasInstanceExtension(VkInstance instance, const std::string &name) const {
    return mImpl->hasInstanceExtension(instance, name);
}
bool ResourceTracker::hasDeviceExtension(VkDevice device, const std::string &name) const {
    return mImpl->hasDeviceExtension(device, name);
}

VkResult ResourceTracker::on_vkEnumerateInstanceVersion(
    void* context,
    VkResult input_result,
    uint32_t* apiVersion) {
    return mImpl->on_vkEnumerateInstanceVersion(context, input_result, apiVersion);
}

VkResult ResourceTracker::on_vkEnumerateInstanceExtensionProperties(
    void* context,
    VkResult input_result,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    return mImpl->on_vkEnumerateInstanceExtensionProperties(
        context, input_result, pLayerName, pPropertyCount, pProperties);
}

VkResult ResourceTracker::on_vkEnumerateDeviceExtensionProperties(
    void* context,
    VkResult input_result,
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    return mImpl->on_vkEnumerateDeviceExtensionProperties(
        context, input_result, physicalDevice, pLayerName, pPropertyCount, pProperties);
}

VkResult ResourceTracker::on_vkEnumeratePhysicalDevices(
    void* context, VkResult input_result,
    VkInstance instance, uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices) {
    return mImpl->on_vkEnumeratePhysicalDevices(
        context, input_result, instance, pPhysicalDeviceCount,
        pPhysicalDevices);
}

void ResourceTracker::on_vkGetPhysicalDeviceMemoryProperties(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    mImpl->on_vkGetPhysicalDeviceMemoryProperties(
        context, physicalDevice, pMemoryProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceMemoryProperties2(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
    mImpl->on_vkGetPhysicalDeviceMemoryProperties2(
        context, physicalDevice, pMemoryProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceMemoryProperties2KHR(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
    mImpl->on_vkGetPhysicalDeviceMemoryProperties2(
        context, physicalDevice, pMemoryProperties);
}

VkResult ResourceTracker::on_vkCreateInstance(
    void* context,
    VkResult input_result,
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    return mImpl->on_vkCreateInstance(
        context, input_result, pCreateInfo, pAllocator, pInstance);
}

VkResult ResourceTracker::on_vkCreateDevice(
    void* context,
    VkResult input_result,
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {
    return mImpl->on_vkCreateDevice(
        context, input_result, physicalDevice, pCreateInfo, pAllocator, pDevice);
}

void ResourceTracker::on_vkDestroyDevice_pre(
    void* context,
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyDevice_pre(context, device, pAllocator);
}

VkResult ResourceTracker::on_vkAllocateMemory(
    void* context,
    VkResult input_result,
    VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory* pMemory) {
    return mImpl->on_vkAllocateMemory(
        context, input_result, device, pAllocateInfo, pAllocator, pMemory);
}

void ResourceTracker::on_vkFreeMemory(
    void* context,
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator) {
    return mImpl->on_vkFreeMemory(
        context, device, memory, pAllocator);
}

VkResult ResourceTracker::on_vkMapMemory(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkDeviceMemory memory,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkMemoryMapFlags flags,
    void** ppData) {
    return mImpl->on_vkMapMemory(
        context, input_result, device, memory, offset, size, flags, ppData);
}

void ResourceTracker::on_vkUnmapMemory(
    void* context,
    VkDevice device,
    VkDeviceMemory memory) {
    mImpl->on_vkUnmapMemory(context, device, memory);
}

VkResult ResourceTracker::on_vkCreateImage(
    void* context, VkResult input_result,
    VkDevice device, const VkImageCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkImage *pImage) {
    return mImpl->on_vkCreateImage(
        context, input_result,
        device, pCreateInfo, pAllocator, pImage);
}

void ResourceTracker::on_vkDestroyImage(
    void* context,
    VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator) {
    mImpl->on_vkDestroyImage(context,
        device, image, pAllocator);
}

void ResourceTracker::on_vkGetImageMemoryRequirements(
    void *context, VkDevice device, VkImage image,
    VkMemoryRequirements *pMemoryRequirements) {
    mImpl->on_vkGetImageMemoryRequirements(
        context, device, image, pMemoryRequirements);
}

void ResourceTracker::on_vkGetImageMemoryRequirements2(
    void *context, VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
    VkMemoryRequirements2 *pMemoryRequirements) {
    mImpl->on_vkGetImageMemoryRequirements2(
        context, device, pInfo, pMemoryRequirements);
}

void ResourceTracker::on_vkGetImageMemoryRequirements2KHR(
    void *context, VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
    VkMemoryRequirements2 *pMemoryRequirements) {
    mImpl->on_vkGetImageMemoryRequirements2KHR(
        context, device, pInfo, pMemoryRequirements);
}

VkResult ResourceTracker::on_vkBindImageMemory(
    void* context, VkResult input_result,
    VkDevice device, VkImage image, VkDeviceMemory memory,
    VkDeviceSize memoryOffset) {
    return mImpl->on_vkBindImageMemory(
        context, input_result, device, image, memory, memoryOffset);
}

VkResult ResourceTracker::on_vkBindImageMemory2(
    void* context, VkResult input_result,
    VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos) {
    return mImpl->on_vkBindImageMemory2(
        context, input_result, device, bindingCount, pBindInfos);
}

VkResult ResourceTracker::on_vkBindImageMemory2KHR(
    void* context, VkResult input_result,
    VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos) {
    return mImpl->on_vkBindImageMemory2KHR(
        context, input_result, device, bindingCount, pBindInfos);
}

VkResult ResourceTracker::on_vkCreateBuffer(
    void* context, VkResult input_result,
    VkDevice device, const VkBufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkBuffer *pBuffer) {
    return mImpl->on_vkCreateBuffer(
        context, input_result,
        device, pCreateInfo, pAllocator, pBuffer);
}

void ResourceTracker::on_vkDestroyBuffer(
    void* context,
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) {
    mImpl->on_vkDestroyBuffer(context, device, buffer, pAllocator);
}

void ResourceTracker::on_vkGetBufferMemoryRequirements(
    void* context, VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements) {
    mImpl->on_vkGetBufferMemoryRequirements(context, device, buffer, pMemoryRequirements);
}

void ResourceTracker::on_vkGetBufferMemoryRequirements2(
    void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements) {
    mImpl->on_vkGetBufferMemoryRequirements2(
        context, device, pInfo, pMemoryRequirements);
}

void ResourceTracker::on_vkGetBufferMemoryRequirements2KHR(
    void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements) {
    mImpl->on_vkGetBufferMemoryRequirements2KHR(
        context, device, pInfo, pMemoryRequirements);
}

VkResult ResourceTracker::on_vkBindBufferMemory(
    void* context, VkResult input_result,
    VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    return mImpl->on_vkBindBufferMemory(
        context, input_result,
        device, buffer, memory, memoryOffset);
}

VkResult ResourceTracker::on_vkBindBufferMemory2(
    void* context, VkResult input_result,
    VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
    return mImpl->on_vkBindBufferMemory2(
        context, input_result,
        device, bindInfoCount, pBindInfos);
}

VkResult ResourceTracker::on_vkBindBufferMemory2KHR(
    void* context, VkResult input_result,
    VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
    return mImpl->on_vkBindBufferMemory2KHR(
        context, input_result,
        device, bindInfoCount, pBindInfos);
}

VkResult ResourceTracker::on_vkCreateSemaphore(
    void* context, VkResult input_result,
    VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSemaphore *pSemaphore) {
    return mImpl->on_vkCreateSemaphore(
        context, input_result,
        device, pCreateInfo, pAllocator, pSemaphore);
}

void ResourceTracker::on_vkDestroySemaphore(
    void* context,
    VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator) {
    mImpl->on_vkDestroySemaphore(context, device, semaphore, pAllocator);
}

VkResult ResourceTracker::on_vkQueueSubmit(
    void* context, VkResult input_result,
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    return mImpl->on_vkQueueSubmit(
        context, input_result, queue, submitCount, pSubmits, fence);
}

void ResourceTracker::unwrap_VkNativeBufferANDROID(
    const VkImageCreateInfo* pCreateInfo,
    VkImageCreateInfo* local_pCreateInfo) {
    mImpl->unwrap_VkNativeBufferANDROID(pCreateInfo, local_pCreateInfo);
}

void ResourceTracker::unwrap_vkAcquireImageANDROID_nativeFenceFd(int fd, int* fd_out) {
    mImpl->unwrap_vkAcquireImageANDROID_nativeFenceFd(fd, fd_out);
}

VkResult ResourceTracker::on_vkMapMemoryIntoAddressSpaceGOOGLE_pre(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkDeviceMemory memory,
    uint64_t* pAddress) {
    return mImpl->on_vkMapMemoryIntoAddressSpaceGOOGLE_pre(
        context, input_result, device, memory, pAddress);
}

VkResult ResourceTracker::on_vkMapMemoryIntoAddressSpaceGOOGLE(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkDeviceMemory memory,
    uint64_t* pAddress) {
    return mImpl->on_vkMapMemoryIntoAddressSpaceGOOGLE(
        context, input_result, device, memory, pAddress);
}

void ResourceTracker::deviceMemoryTransform_tohost(
    VkDeviceMemory* memory, uint32_t memoryCount,
    VkDeviceSize* offset, uint32_t offsetCount,
    VkDeviceSize* size, uint32_t sizeCount,
    uint32_t* typeIndex, uint32_t typeIndexCount,
    uint32_t* typeBits, uint32_t typeBitsCount) {
    mImpl->deviceMemoryTransform_tohost(
        memory, memoryCount,
        offset, offsetCount,
        size, sizeCount,
        typeIndex, typeIndexCount,
        typeBits, typeBitsCount);
}

void ResourceTracker::deviceMemoryTransform_fromhost(
    VkDeviceMemory* memory, uint32_t memoryCount,
    VkDeviceSize* offset, uint32_t offsetCount,
    VkDeviceSize* size, uint32_t sizeCount,
    uint32_t* typeIndex, uint32_t typeIndexCount,
    uint32_t* typeBits, uint32_t typeBitsCount) {
    mImpl->deviceMemoryTransform_fromhost(
        memory, memoryCount,
        offset, offsetCount,
        size, sizeCount,
        typeIndex, typeIndexCount,
        typeBits, typeBitsCount);
}

#define DEFINE_TRANSFORMED_TYPE_IMPL(type) \
    void ResourceTracker::transformImpl_##type##_tohost(const type*, uint32_t) { } \
    void ResourceTracker::transformImpl_##type##_fromhost(const type*, uint32_t) { } \

LIST_TRANSFORMED_TYPES(DEFINE_TRANSFORMED_TYPE_IMPL)

} // namespace goldfish_vk
