// Copyright 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either expresso or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "VkDecoderGlobalState.h"

#include <algorithm>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ExternalObjectManager.h"
#include "RenderThreadInfoVk.h"
#include "VkAndroidNativeBuffer.h"
#include "VkCommonOperations.h"
#include "VkDecoderContext.h"
#include "VkDecoderInternalStructs.h"
#include "VkDecoderSnapshot.h"
#include "VkDecoderSnapshotUtils.h"
#include "VkEmulatedPhysicalDeviceMemory.h"
#include "VulkanDispatch.h"
#include "VulkanStream.h"
#include "aemu/base/ManagedDescriptor.hpp"
#include "aemu/base/Optional.h"
#include "aemu/base/containers/EntityManager.h"
#include "aemu/base/containers/HybridEntityManager.h"
#include "aemu/base/containers/Lookup.h"
#include "aemu/base/files/Stream.h"
#include "aemu/base/memory/SharedMemory.h"
#include "aemu/base/synchronization/ConditionVariable.h"
#include "aemu/base/synchronization/Lock.h"
#include "aemu/base/system/System.h"
#include "common/goldfish_vk_deepcopy.h"
#include "common/goldfish_vk_dispatch.h"
#include "common/goldfish_vk_marshaling.h"
#include "common/goldfish_vk_reserved_marshaling.h"
#include "compressedTextureFormats/AstcCpuDecompressor.h"
#include "gfxstream/host/Tracing.h"
#include "host-common/GfxstreamFatalError.h"
#include "host-common/HostmemIdMapping.h"
#include "host-common/address_space_device_control_ops.h"
#include "host-common/emugl_vm_operations.h"
#include "host-common/vm_operations.h"
#include "utils/RenderDoc.h"
#include "vk_util.h"
#include "vulkan/VkFormatUtils.h"
#include "vulkan/emulated_textures/AstcTexture.h"
#include "vulkan/emulated_textures/CompressedImageInfo.h"
#include "vulkan/emulated_textures/GpuDecompressionPipeline.h"
#include "vulkan/vk_enum_string_helper.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <vulkan/vulkan_beta.h> // for MoltenVK portability extensions
#endif

#ifndef VERBOSE
#define VERBOSE(fmt, ...)        \
    if (android::base::isVerboseLogging()) \
        fprintf(stderr, "%s:%d " fmt "\n", __func__, __LINE__, ##__VA_ARGS__);
#endif

#include <climits>

namespace gfxstream {
namespace vk {

using android::base::AutoLock;
using android::base::ConditionVariable;
using android::base::DescriptorType;
using android::base::Lock;
using android::base::ManagedDescriptor;
using android::base::MetricEventBadPacketLength;
using android::base::MetricEventDuplicateSequenceNum;
using android::base::MetricEventVulkanOutOfMemory;
using android::base::Optional;
using android::base::SharedMemory;
using android::base::StaticLock;
using emugl::ABORT_REASON_OTHER;
using emugl::FatalError;
using emugl::GfxApiLogger;
using gfxstream::ExternalObjectManager;
using gfxstream::VulkanInfo;

// TODO(b/261477138): Move to a shared aemu definition
#define __ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))
#define __ALIGN(x, a) __ALIGN_MASK(x, (__typeof__(x))(a)-1)

// TODO: Asserts build
#define DCHECK(condition) (void)(condition);

#define VKDGS_DEBUG 0

#if VKDGS_DEBUG
#define VKDGS_LOG(fmt, ...) fprintf(stderr, "%s:%d " fmt "\n", __func__, __LINE__, ##__VA_ARGS__);
#else
#define VKDGS_LOG(fmt, ...)
#endif

// Blob mem
#define STREAM_BLOB_MEM_GUEST 1
#define STREAM_BLOB_MEM_HOST3D 2
#define STREAM_BLOB_MEM_HOST3D_GUEST 3

// Blob flags
#define STREAM_BLOB_FLAG_USE_MAPPABLE 1
#define STREAM_BLOB_FLAG_USE_SHAREABLE 2
#define STREAM_BLOB_FLAG_USE_CROSS_DEVICE 4
#define STREAM_BLOB_FLAG_CREATE_GUEST_HANDLE 8

#define VALIDATE_REQUIRED_HANDLE(parameter) \
    validateRequiredHandle(__FUNCTION__, #parameter, parameter)

template <typename T>
void validateRequiredHandle(const char* api_name, const char* parameter_name, T value) {
    if (value == VK_NULL_HANDLE) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << api_name << ":" << parameter_name;
    }
}

VK_EXT_SYNC_HANDLE dupExternalSync(VK_EXT_SYNC_HANDLE h) {
#ifdef _WIN32
    auto myProcessHandle = GetCurrentProcess();
    VK_EXT_SYNC_HANDLE res;
    DuplicateHandle(myProcessHandle, h,     // source process and handle
                    myProcessHandle, &res,  // target process and pointer to handle
                    0 /* desired access (ignored) */, true /* inherit */,
                    DUPLICATE_SAME_ACCESS /* same access option */);
    return res;
#else
    return dup(h);
#endif
}

// A list of device extensions that should not be passed to the host driver.
// These will mainly include Vulkan features that we emulate ourselves.
static constexpr const char* const kEmulatedDeviceExtensions[] = {
    "VK_ANDROID_external_memory_android_hardware_buffer",
    "VK_ANDROID_native_buffer",
    "VK_FUCHSIA_buffer_collection",
    "VK_FUCHSIA_external_memory",
    "VK_FUCHSIA_external_semaphore",
    VK_EXT_DEVICE_MEMORY_REPORT_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
#if defined(__QNX__)
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
#endif
};

// A list of instance extensions that should not be passed to the host driver.
// On older pre-1.1 Vulkan platforms, gfxstream emulates these features.
static constexpr const char* const kEmulatedInstanceExtensions[] = {
    VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
};

static constexpr uint32_t kMaxSafeVersion = VK_MAKE_VERSION(1, 3, 0);
static constexpr uint32_t kMinVersion = VK_MAKE_VERSION(1, 0, 0);

static constexpr uint64_t kPageSizeforBlob = 4096;
static constexpr uint64_t kPageMaskForBlob = ~(0xfff);

static uint64_t hostBlobId = 0;

// b/319729462
// On snapshot load, thread local data is not available, thus we use a
// fake context ID. We will eventually need to fix it once we start using
// snapshot with virtio.
static uint32_t kTemporaryContextIdForSnapshotLoading = 1;

static std::unordered_set<std::string> kSnapshotAppAllowList = {"Chromium"};
static std::unordered_set<std::string> kSnapshotEngineAllowList = {"ANGLE"};

#define DEFINE_BOXED_HANDLE_TYPE_TAG(type) Tag_##type,

enum BoxedHandleTypeTag {
    Tag_Invalid = 0,
    GOLDFISH_VK_LIST_HANDLE_TYPES_BY_STAGE(DEFINE_BOXED_HANDLE_TYPE_TAG)
};

template <class T>
class BoxedHandleManager {
   public:
    // The hybrid entity manager uses a sequence lock to protect access to
    // a working set of 16000 handles, allowing us to avoid using a regular
    // lock for those. Performance is degraded when going over this number,
    // as it will then fall back to a std::map.
    //
    // We use 16000 as the max number of live handles to track; we don't
    // expect the system to go over 16000 total live handles, outside some
    // dEQP object management tests.
    using Store = android::base::HybridEntityManager<16000, uint64_t, T>;

    Lock lock;
    mutable Store store;
    std::unordered_map<uint64_t, uint64_t> reverseMap;
    struct DelayedRemove {
        uint64_t handle;
        std::function<void()> callback;
    };
    std::unordered_map<VkDevice, std::vector<DelayedRemove>> delayedRemoves;

    void clear() {
        reverseMap.clear();
        store.clear();
    }

    uint64_t add(const T& item, BoxedHandleTypeTag tag) {
        auto res = (uint64_t)store.add(item, (size_t)tag);
        AutoLock l(lock);
        reverseMap[(uint64_t)(item.underlying)] = res;
        return res;
    }

    uint64_t addFixed(uint64_t handle, const T& item, BoxedHandleTypeTag tag) {
        auto res = (uint64_t)store.addFixed(handle, item, (size_t)tag);
        AutoLock l(lock);
        reverseMap[(uint64_t)(item.underlying)] = res;
        return res;
    }

    void update(uint64_t handle, const T& item, BoxedHandleTypeTag tag) {
        auto storedItem = store.get(handle);
        uint64_t oldHandle = (uint64_t)storedItem->underlying;
        *storedItem = item;
        AutoLock l(lock);
        if (oldHandle) {
            reverseMap.erase(oldHandle);
        }
        reverseMap[(uint64_t)(item.underlying)] = handle;
    }

    void remove(uint64_t h) {
        auto item = get(h);
        if (item) {
            AutoLock l(lock);
            reverseMap.erase((uint64_t)(item->underlying));
        }
        store.remove(h);
    }

    void removeDelayed(uint64_t h, VkDevice device, std::function<void()> callback) {
        AutoLock l(lock);
        delayedRemoves[device].push_back({h, callback});
    }

    void processDelayedRemovesGlobalStateLocked(VkDevice device) {
        AutoLock l(lock);
        auto it = delayedRemoves.find(device);
        if (it == delayedRemoves.end()) return;
        auto& delayedRemovesList = it->second;
        for (const auto& r : delayedRemovesList) {
            auto h = r.handle;
            // VkDecoderGlobalState is already locked when callback is called.
            if (r.callback) {
                r.callback();
            }
            store.remove(h);
        }
        delayedRemovesList.clear();
        delayedRemoves.erase(it);
    }

    T* get(uint64_t h) { return (T*)store.get_const(h); }

    uint64_t getBoxedFromUnboxedLocked(uint64_t unboxed) {
        auto* res = android::base::find(reverseMap, unboxed);
        if (!res) return 0;
        return *res;
    }
};

struct OrderMaintenanceInfo {
    uint32_t sequenceNumber = 0;
    Lock lock;
    ConditionVariable cv;

    uint32_t refcount = 1;

    void incRef() { __atomic_add_fetch(&refcount, 1, __ATOMIC_SEQ_CST); }

    bool decRef() { return 0 == __atomic_sub_fetch(&refcount, 1, __ATOMIC_SEQ_CST); }
};

static void acquireOrderMaintInfo(OrderMaintenanceInfo* ord) {
    if (!ord) return;
    ord->incRef();
}

static void releaseOrderMaintInfo(OrderMaintenanceInfo* ord) {
    if (!ord) return;
    if (ord->decRef()) delete ord;
}

template <class T>
class DispatchableHandleInfo {
   public:
    T underlying;
    VulkanDispatch* dispatch = nullptr;
    bool ownDispatch = false;
    OrderMaintenanceInfo* ordMaintInfo = nullptr;
    VulkanMemReadingStream* readStream = nullptr;
};

static BoxedHandleManager<DispatchableHandleInfo<uint64_t>> sBoxedHandleManager;

struct ReadStreamRegistry {
    Lock mLock;

    std::vector<VulkanMemReadingStream*> freeStreams;

    ReadStreamRegistry() { freeStreams.reserve(100); };

    VulkanMemReadingStream* pop(const gfxstream::host::FeatureSet& features) {
        AutoLock lock(mLock);
        if (freeStreams.empty()) {
            return new VulkanMemReadingStream(nullptr, features);
        } else {
            VulkanMemReadingStream* res = freeStreams.back();
            freeStreams.pop_back();
            return res;
        }
    }

    void push(VulkanMemReadingStream* stream) {
        AutoLock lock(mLock);
        freeStreams.push_back(stream);
    }
};

static ReadStreamRegistry sReadStreamRegistry;

class VkDecoderGlobalState::Impl {
   public:
    Impl()
        : m_vk(vkDispatch()),
          m_emu(getGlobalVkEmulation()),
          mRenderDocWithMultipleVkInstances(m_emu->guestRenderDoc.get()) {
        mSnapshotsEnabled = m_emu->features.VulkanSnapshots.enabled;
        mVkCleanupEnabled =
            android::base::getEnvironmentVariable("ANDROID_EMU_VK_NO_CLEANUP") != "1";
        mLogging = android::base::getEnvironmentVariable("ANDROID_EMU_VK_LOG_CALLS") == "1";
        mVerbosePrints = android::base::getEnvironmentVariable("ANDROID_EMUGL_VERBOSE") == "1";
        if (get_emugl_address_space_device_control_ops().control_get_hw_funcs &&
            get_emugl_address_space_device_control_ops().control_get_hw_funcs()) {
            mUseOldMemoryCleanupPath = 0 == get_emugl_address_space_device_control_ops()
                                                .control_get_hw_funcs()
                                                ->getPhysAddrStartLocked();
        }
    }

    ~Impl() = default;

    // Resets all internal tracking info.
    // Assumes that the heavyweight cleanup operations
    // have already happened.
    void clear() {
        mInstanceInfo.clear();
        mPhysdevInfo.clear();
        mDeviceInfo.clear();
        mImageInfo.clear();
        mImageViewInfo.clear();
        mSamplerInfo.clear();
        mCommandBufferInfo.clear();
        mCommandPoolInfo.clear();
        mDeviceToPhysicalDevice.clear();
        mPhysicalDeviceToInstance.clear();
        mQueueInfo.clear();
        mBufferInfo.clear();
        mMemoryInfo.clear();
        mShaderModuleInfo.clear();
        mPipelineCacheInfo.clear();
        mPipelineInfo.clear();
        mRenderPassInfo.clear();
        mFramebufferInfo.clear();
        mSemaphoreInfo.clear();
        mFenceInfo.clear();
#ifdef _WIN32
        mSemaphoreId = 1;
        mExternalSemaphoresById.clear();
#endif
        mDescriptorUpdateTemplateInfo.clear();

        mCreatedHandlesForSnapshotLoad.clear();
        mCreatedHandlesForSnapshotLoadIndex = 0;

        sBoxedHandleManager.clear();
    }

    bool snapshotsEnabled() const { return mSnapshotsEnabled; }

    bool vkCleanupEnabled() const { return mVkCleanupEnabled; }

    const gfxstream::host::FeatureSet& getFeatures() const { return m_emu->features; }

    StateBlock createSnapshotStateBlock(VkDevice unboxed_device) {
            const auto& device = unboxed_device;
            const auto& deviceInfo = android::base::find(mDeviceInfo, device);
            const auto physicalDevice = deviceInfo->physicalDevice;
            const auto& physicalDeviceInfo = android::base::find(mPhysdevInfo, physicalDevice);
            const auto& instanceInfo = android::base::find(mInstanceInfo, physicalDeviceInfo->instance);

            VulkanDispatch* ivk = dispatch_VkInstance(instanceInfo->boxed);
            VulkanDispatch* dvk = dispatch_VkDevice(deviceInfo->boxed);

            StateBlock stateBlock{
                .physicalDevice = physicalDevice,
                .physicalDeviceInfo = physicalDeviceInfo,
                .device = device,
                .deviceDispatch = dvk,
                .queue = VK_NULL_HANDLE,
                .commandPool = VK_NULL_HANDLE,
            };

            uint32_t queueFamilyCount = 0;
            ivk->vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                                          nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilyProps(queueFamilyCount);
            ivk->vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                                          queueFamilyProps.data());
            uint32_t queueFamilyIndex = 0;
            for (auto queue : deviceInfo->queues) {
                int idx = queue.first;
                if ((queueFamilyProps[idx].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                    continue;
                }
                stateBlock.queue = queue.second[0];
                queueFamilyIndex = idx;
                break;
            }

            VkCommandPoolCreateInfo commandPoolCi = {
                VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                0,
                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                queueFamilyIndex,
            };
            dvk->vkCreateCommandPool(device, &commandPoolCi, nullptr, &stateBlock.commandPool);
            return stateBlock;
    }

    void releaseSnapshotStateBlock(const StateBlock* stateBlock) {
        stateBlock->deviceDispatch->vkDestroyCommandPool(stateBlock->device, stateBlock->commandPool, nullptr);
    }

    void save(android::base::Stream* stream) {
        mSnapshotState = SnapshotState::Saving;

#ifdef GFXSTREAM_ENABLE_HOST_VK_SNAPSHOT
        if (!mInstanceInfo.empty()) {
            get_emugl_vm_operations().setStatSnapshotUseVulkan();
        }
#endif

        snapshot()->save(stream);
        // Save mapped memory
        uint32_t memoryCount = 0;
        for (const auto& it : mMemoryInfo) {
            if (it.second.ptr) {
                memoryCount++;
            }
        }
        stream->putBe32(memoryCount);
        for (const auto& it : mMemoryInfo) {
            if (!it.second.ptr) {
                continue;
            }
            stream->putBe64(reinterpret_cast<uint64_t>(
                unboxed_to_boxed_non_dispatchable_VkDeviceMemory(it.first)));
            stream->putBe64(it.second.size);
            stream->write(it.second.ptr, it.second.size);
        }

        // Set up VK structs to snapshot other Vulkan objects
        // TODO(b/323064243): group all images from the same device and reuse queue / command pool

        std::vector<VkImage> sortedBoxedImages;
        for (const auto& imageIte : mImageInfo) {
            sortedBoxedImages.push_back(unboxed_to_boxed_non_dispatchable_VkImage(imageIte.first));
        }
        // Image contents need to be saved and loaded in the same order.
        // So sort them (by boxed handles) first.
        std::sort(sortedBoxedImages.begin(), sortedBoxedImages.end());
        for (const auto& boxedImage : sortedBoxedImages) {
            auto unboxedImage = unbox_VkImage(boxedImage);
            const ImageInfo& imageInfo = mImageInfo[unboxedImage];
            if (imageInfo.memory == VK_NULL_HANDLE) {
                continue;
            }
            // Vulkan command playback doesn't recover image layout. We need to do it here.
            stream->putBe32(imageInfo.layout);

            StateBlock stateBlock = createSnapshotStateBlock(imageInfo.device);
            // TODO(b/294277842): make sure the queue is empty before using.
            saveImageContent(stream, &stateBlock, unboxedImage, &imageInfo);
            releaseSnapshotStateBlock(&stateBlock);
        }

        // snapshot buffers
        std::vector<VkBuffer> sortedBoxedBuffers;
        for (const auto& bufferIte : mBufferInfo) {
            sortedBoxedBuffers.push_back(
                unboxed_to_boxed_non_dispatchable_VkBuffer(bufferIte.first));
        }
        sort(sortedBoxedBuffers.begin(), sortedBoxedBuffers.end());
        for (const auto& boxedBuffer : sortedBoxedBuffers) {
            auto unboxedBuffer = unbox_VkBuffer(boxedBuffer);
            const BufferInfo& bufferInfo = mBufferInfo[unboxedBuffer];
            if (bufferInfo.memory == VK_NULL_HANDLE) {
                continue;
            }
            // TODO: add a special case for host mapped memory
            StateBlock stateBlock = createSnapshotStateBlock(bufferInfo.device);

            // TODO(b/294277842): make sure the queue is empty before using.
            saveBufferContent(stream, &stateBlock, unboxedBuffer, &bufferInfo);
            releaseSnapshotStateBlock(&stateBlock);
        }

        // snapshot descriptors
        std::vector<VkDescriptorPool> sortedBoxedDescriptorPools;
        for (const auto& descriptorPoolIte : mDescriptorPoolInfo) {
            auto boxed =
                unboxed_to_boxed_non_dispatchable_VkDescriptorPool(descriptorPoolIte.first);
            sortedBoxedDescriptorPools.push_back(boxed);
        }
        std::sort(sortedBoxedDescriptorPools.begin(), sortedBoxedDescriptorPools.end());
        for (const auto& boxedDescriptorPool : sortedBoxedDescriptorPools) {
            auto unboxedDescriptorPool = unbox_VkDescriptorPool(boxedDescriptorPool);
            const DescriptorPoolInfo& poolInfo = mDescriptorPoolInfo[unboxedDescriptorPool];

            for (uint64_t poolId : poolInfo.poolIds) {
                DispatchableHandleInfo<uint64_t>* setHandleInfo = sBoxedHandleManager.get(poolId);
                bool allocated = setHandleInfo->underlying != 0;
                stream->putByte(allocated);
                if (!allocated) {
                    continue;
                }

                const DescriptorSetInfo& descriptorSetInfo =
                    mDescriptorSetInfo[(VkDescriptorSet)setHandleInfo->underlying];
                VkDescriptorSetLayout boxedLayout =
                    unboxed_to_boxed_non_dispatchable_VkDescriptorSetLayout(
                        descriptorSetInfo.unboxedLayout);
                stream->putBe64((uint64_t)boxedLayout);
                // Count all valid descriptors.
                //
                // There is a use case where user can create an image, write it to a descriptor,
                // read/write the image by committing a command, then delete the image without
                // unbinding the descriptor. For example:
                //
                // T1: create "vkimage1" (original)
                // T2: update binding1 of vkdescriptorset1 with vkimage1
                // T3: draw
                // T4: delete "vkimage1" (original)
                // T5: create "vkimage1" (recycled)
                // T6: snapshot load
                //
                // At the point of the snapshot, the original vk image has been invalidated,
                // thus we cannot call vkUpdateDescriptorSets for it, and need to remove it
                // from the snapshot.
                //
                // The current implementation bases on smart pointers. A descriptor set info
                // holds weak pointers to their underlying resources (image, image view, buffer).
                // On snapshot load, we check if any of the smart pointers are invalidated.
                //
                // An alternative approach has been discussed by, instead of using smart
                // pointers, checking valid handles on snapshot save. This approach has the
                // advantage that it reduces number of smart pointer allocations. After discussion
                // we concluded that there is at least one corner case that will break the
                // alternative approach. That is when the user deletes a bound vkimage and creates
                // a new vkimage. The driver is free to reuse released handles, thus we might
                // end up having a new vkimage with the same handle as the old one (see T5 in the
                // example), and think the binding is still valid. And if we bind the new image
                // regardless, we might hit a Vulkan validation error because the new image might
                // have the "usage" flag that is unsuitable to bind to descriptors.
                std::vector<std::pair<int, int>> validWriteIndices;
                for (int bindingIdx = 0; bindingIdx < descriptorSetInfo.allWrites.size();
                     bindingIdx++) {
                    for (int bindingElemIdx = 0;
                         bindingElemIdx < descriptorSetInfo.allWrites[bindingIdx].size();
                         bindingElemIdx++) {
                        const auto& entry = descriptorSetInfo.allWrites[bindingIdx][bindingElemIdx];
                        if (entry.writeType == DescriptorSetInfo::DescriptorWriteType::Empty) {
                            continue;
                        }
                        int dependencyObjCount =
                            descriptorDependencyObjectCount(entry.descriptorType);
                        if (entry.alives.size() < dependencyObjCount) {
                            continue;
                        }
                        bool isValid = true;
                        for (const auto& alive : entry.alives) {
                            isValid &= !alive.expired();
                            if (!isValid) {
                                break;
                            }
                        }
                        if (!isValid) {
                            continue;
                        }
                        validWriteIndices.push_back(std::make_pair(bindingIdx, bindingElemIdx));
                    }
                }
                stream->putBe64(validWriteIndices.size());
                // Save all valid descriptors
                for (const auto& idx : validWriteIndices) {
                    const auto& entry = descriptorSetInfo.allWrites[idx.first][idx.second];
                    stream->putBe32(idx.first);
                    stream->putBe32(idx.second);
                    stream->putBe32(entry.writeType);
                    // entry.descriptorType might be redundant.
                    stream->putBe32(entry.descriptorType);
                    switch (entry.writeType) {
                        case DescriptorSetInfo::DescriptorWriteType::ImageInfo: {
                            VkDescriptorImageInfo imageInfo = entry.imageInfo;
                            // Get the unboxed version
                            imageInfo.imageView =
                                descriptorTypeContainsImage(entry.descriptorType)
                                    ? unboxed_to_boxed_non_dispatchable_VkImageView(
                                          imageInfo.imageView)
                                    : VK_NULL_HANDLE;
                            imageInfo.sampler =
                                descriptorTypeContainsSampler(entry.descriptorType)
                                    ? unboxed_to_boxed_non_dispatchable_VkSampler(imageInfo.sampler)
                                    : VK_NULL_HANDLE;
                            stream->write(&imageInfo, sizeof(imageInfo));
                        } break;
                        case DescriptorSetInfo::DescriptorWriteType::BufferInfo: {
                            VkDescriptorBufferInfo bufferInfo = entry.bufferInfo;
                            // Get the unboxed version
                            bufferInfo.buffer =
                                unboxed_to_boxed_non_dispatchable_VkBuffer(bufferInfo.buffer);
                            stream->write(&bufferInfo, sizeof(bufferInfo));
                        } break;
                        case DescriptorSetInfo::DescriptorWriteType::BufferView: {
                            // Get the unboxed version
                            VkBufferView bufferView =
                                unboxed_to_boxed_non_dispatchable_VkBufferView(entry.bufferView);
                            stream->write(&bufferView, sizeof(bufferView));
                        } break;
                        case DescriptorSetInfo::DescriptorWriteType::InlineUniformBlock:
                        case DescriptorSetInfo::DescriptorWriteType::AccelerationStructure:
                            // TODO
                            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                                << "Encountered pending inline uniform block or acceleration "
                                   "structure "
                                   "desc write, abort (NYI)";
                        default:
                            break;
                    }
                }
            }
        }

        // Fences
        std::vector<VkFence> unsignaledFencesBoxed;
        for (const auto& fence : mFenceInfo) {
            if (!fence.second.boxed) {
                continue;
            }
            const auto& device = fence.second.device;
            const auto& deviceInfo = android::base::find(mDeviceInfo, device);
            VulkanDispatch* dvk = dispatch_VkDevice(deviceInfo->boxed);
            if (VK_NOT_READY == dvk->vkGetFenceStatus(device, fence.first)) {
                unsignaledFencesBoxed.push_back(fence.second.boxed);
            }
        }
        stream->putBe64(unsignaledFencesBoxed.size());
        stream->write(unsignaledFencesBoxed.data(), unsignaledFencesBoxed.size() * sizeof(VkFence));
        mSnapshotState = SnapshotState::Normal;
    }

    void load(android::base::Stream* stream, GfxApiLogger& gfxLogger,
              HealthMonitor<>* healthMonitor) {
        // assume that we already destroyed all instances
        // from FrameBuffer's onLoad method.

        // destroy all current internal data structures
        clear();
        mSnapshotState = SnapshotState::Loading;
        android::base::BumpPool bumpPool;
        // this part will replay in the decoder
        snapshot()->load(stream, gfxLogger, healthMonitor);
        // load mapped memory
        uint32_t memoryCount = stream->getBe32();
        for (uint32_t i = 0; i < memoryCount; i++) {
            VkDeviceMemory boxedMemory = reinterpret_cast<VkDeviceMemory>(stream->getBe64());
            VkDeviceMemory unboxedMemory = unbox_VkDeviceMemory(boxedMemory);
            auto it = mMemoryInfo.find(unboxedMemory);
            if (it == mMemoryInfo.end()) {
                GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                    << "Snapshot load failure: cannot find memory handle for " << boxedMemory;
            }
            VkDeviceSize size = stream->getBe64();
            if (size != it->second.size || !it->second.ptr) {
                GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                    << "Snapshot load failure: memory size does not match for " << boxedMemory;
            }
            stream->read(it->second.ptr, size);
        }
        // Set up VK structs to snapshot other Vulkan objects
        // TODO(b/323064243): group all images from the same device and reuse queue / command pool

        std::vector<VkImage> sortedBoxedImages;
        for (const auto& imageIte : mImageInfo) {
            sortedBoxedImages.push_back(unboxed_to_boxed_non_dispatchable_VkImage(imageIte.first));
        }
        sort(sortedBoxedImages.begin(), sortedBoxedImages.end());
        for (const auto& boxedImage : sortedBoxedImages) {
            auto unboxedImage = unbox_VkImage(boxedImage);
            ImageInfo& imageInfo = mImageInfo[unboxedImage];
            if (imageInfo.memory == VK_NULL_HANDLE) {
                continue;
            }
            // Playback doesn't recover image layout. We need to do it here.
            //
            // Layout transform was done by vkCmdPipelineBarrier but we don't record such command
            // directly. Instead, we memorize the current layout and add our own
            // vkCmdPipelineBarrier after load.
            //
            // We do the layout transform in loadImageContent. There are still use cases where it
            // should recover the layout but does not.
            //
            // TODO(b/323059453): fix corner cases when image contents cannot be properly loaded.
            imageInfo.layout = static_cast<VkImageLayout>(stream->getBe32());
            StateBlock stateBlock = createSnapshotStateBlock(imageInfo.device);
            // TODO(b/294277842): make sure the queue is empty before using.
            loadImageContent(stream, &stateBlock, unboxedImage, &imageInfo);
            releaseSnapshotStateBlock(&stateBlock);
        }

        // snapshot buffers
        std::vector<VkBuffer> sortedBoxedBuffers;
        for (const auto& bufferIte : mBufferInfo) {
            sortedBoxedBuffers.push_back(
                unboxed_to_boxed_non_dispatchable_VkBuffer(bufferIte.first));
        }
        sort(sortedBoxedBuffers.begin(), sortedBoxedBuffers.end());
        for (const auto& boxedBuffer : sortedBoxedBuffers) {
            auto unboxedBuffer = unbox_VkBuffer(boxedBuffer);
            const BufferInfo& bufferInfo = mBufferInfo[unboxedBuffer];
            if (bufferInfo.memory == VK_NULL_HANDLE) {
                continue;
            }
            // TODO: add a special case for host mapped memory
            StateBlock stateBlock = createSnapshotStateBlock(bufferInfo.device);
            // TODO(b/294277842): make sure the queue is empty before using.
            loadBufferContent(stream, &stateBlock, unboxedBuffer, &bufferInfo);
            releaseSnapshotStateBlock(&stateBlock);
        }

        // snapshot descriptors
        std::vector<VkDescriptorPool> sortedBoxedDescriptorPools;
        for (const auto& descriptorPoolIte : mDescriptorPoolInfo) {
            auto boxed =
                unboxed_to_boxed_non_dispatchable_VkDescriptorPool(descriptorPoolIte.first);
            sortedBoxedDescriptorPools.push_back(boxed);
        }
        sort(sortedBoxedDescriptorPools.begin(), sortedBoxedDescriptorPools.end());
        for (const auto& boxedDescriptorPool : sortedBoxedDescriptorPools) {
            auto unboxedDescriptorPool = unbox_VkDescriptorPool(boxedDescriptorPool);
            const DescriptorPoolInfo& poolInfo = mDescriptorPoolInfo[unboxedDescriptorPool];

            std::vector<VkDescriptorSetLayout> layouts;
            std::vector<uint64_t> poolIds;
            std::vector<VkWriteDescriptorSet> writeDescriptorSets;
            std::vector<uint32_t> writeStartingIndices;

            // Temporary structures for the pointers in VkWriteDescriptorSet.
            // Use unique_ptr so that the pointers don't change when vector resizes.
            std::vector<std::unique_ptr<VkDescriptorImageInfo>> tmpImageInfos;
            std::vector<std::unique_ptr<VkDescriptorBufferInfo>> tmpBufferInfos;
            std::vector<std::unique_ptr<VkBufferView>> tmpBufferViews;

            for (uint64_t poolId : poolInfo.poolIds) {
                bool allocated = stream->getByte();
                if (!allocated) {
                    continue;
                }
                poolIds.push_back(poolId);
                writeStartingIndices.push_back(writeDescriptorSets.size());
                VkDescriptorSetLayout boxedLayout = (VkDescriptorSetLayout)stream->getBe64();
                layouts.push_back(unbox_VkDescriptorSetLayout(boxedLayout));
                uint64_t validWriteCount = stream->getBe64();
                for (int write = 0; write < validWriteCount; write++) {
                    uint32_t binding = stream->getBe32();
                    uint32_t arrayElement = stream->getBe32();
                    DescriptorSetInfo::DescriptorWriteType writeType =
                        static_cast<DescriptorSetInfo::DescriptorWriteType>(stream->getBe32());
                    VkDescriptorType descriptorType =
                        static_cast<VkDescriptorType>(stream->getBe32());
                    VkWriteDescriptorSet writeDescriptorSet = {
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = (VkDescriptorSet)poolId,
                        .dstBinding = binding,
                        .dstArrayElement = arrayElement,
                        .descriptorCount = 1,
                        .descriptorType = descriptorType,
                    };
                    switch (writeType) {
                        case DescriptorSetInfo::DescriptorWriteType::ImageInfo: {
                            tmpImageInfos.push_back(std::make_unique<VkDescriptorImageInfo>());
                            writeDescriptorSet.pImageInfo = tmpImageInfos.back().get();
                            VkDescriptorImageInfo& imageInfo = *tmpImageInfos.back();
                            stream->read(&imageInfo, sizeof(imageInfo));
                            imageInfo.imageView = descriptorTypeContainsImage(descriptorType)
                                                      ? unbox_VkImageView(imageInfo.imageView)
                                                      : 0;
                            imageInfo.sampler = descriptorTypeContainsSampler(descriptorType)
                                                    ? unbox_VkSampler(imageInfo.sampler)
                                                    : 0;
                        } break;
                        case DescriptorSetInfo::DescriptorWriteType::BufferInfo: {
                            tmpBufferInfos.push_back(std::make_unique<VkDescriptorBufferInfo>());
                            writeDescriptorSet.pBufferInfo = tmpBufferInfos.back().get();
                            VkDescriptorBufferInfo& bufferInfo = *tmpBufferInfos.back();
                            stream->read(&bufferInfo, sizeof(bufferInfo));
                            bufferInfo.buffer = unbox_VkBuffer(bufferInfo.buffer);
                        } break;
                        case DescriptorSetInfo::DescriptorWriteType::BufferView: {
                            tmpBufferViews.push_back(std::make_unique<VkBufferView>());
                            writeDescriptorSet.pTexelBufferView = tmpBufferViews.back().get();
                            VkBufferView& bufferView = *tmpBufferViews.back();
                            stream->read(&bufferView, sizeof(bufferView));
                            bufferView = unbox_VkBufferView(bufferView);
                        } break;
                        case DescriptorSetInfo::DescriptorWriteType::InlineUniformBlock:
                        case DescriptorSetInfo::DescriptorWriteType::AccelerationStructure:
                            // TODO
                            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                                << "Encountered pending inline uniform block or acceleration "
                                   "structure "
                                   "desc write, abort (NYI)";
                        default:
                            break;
                    }
                    writeDescriptorSets.push_back(writeDescriptorSet);
                }
            }
            std::vector<uint32_t> whichPool(poolIds.size(), 0);
            std::vector<uint32_t> pendingAlloc(poolIds.size(), true);

            const auto& device = poolInfo.device;
            const auto& deviceInfo = android::base::find(mDeviceInfo, device);
            VulkanDispatch* dvk = dispatch_VkDevice(deviceInfo->boxed);
            on_vkQueueCommitDescriptorSetUpdatesGOOGLE(
                &bumpPool, dvk, device, 1, &unboxedDescriptorPool, poolIds.size(), layouts.data(),
                poolIds.data(), whichPool.data(), pendingAlloc.data(), writeStartingIndices.data(),
                writeDescriptorSets.size(), writeDescriptorSets.data());
        }
        // Fences
        uint64_t fenceCount = stream->getBe64();
        std::vector<VkFence> unsignaledFencesBoxed(fenceCount);
        stream->read(unsignaledFencesBoxed.data(), fenceCount * sizeof(VkFence));
        for (VkFence boxedFence : unsignaledFencesBoxed) {
            VkFence unboxedFence = unbox_VkFence(boxedFence);
            auto it = mFenceInfo.find(unboxedFence);
            if (it == mFenceInfo.end()) {
                GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                    << "Snapshot load failure: unrecognized VkFence";
            }
            const auto& device = it->second.device;
            const auto& deviceInfo = android::base::find(mDeviceInfo, device);
            VulkanDispatch* dvk = dispatch_VkDevice(deviceInfo->boxed);
            dvk->vkResetFences(device, 1, &unboxedFence);
        }
#ifdef GFXSTREAM_ENABLE_HOST_VK_SNAPSHOT
        if (!mInstanceInfo.empty()) {
            get_emugl_vm_operations().setStatSnapshotUseVulkan();
        }
#endif

        mSnapshotState = SnapshotState::Normal;
    }

    void lock() { mLock.lock(); }

    void unlock() { mLock.unlock(); }

    size_t setCreatedHandlesForSnapshotLoad(const unsigned char* buffer) {
        size_t consumed = 0;

        if (!buffer) return consumed;

        uint32_t bufferSize = *(uint32_t*)buffer;

        consumed += 4;

        uint32_t handleCount = bufferSize / 8;
        VKDGS_LOG("incoming handle count: %u", handleCount);

        uint64_t* handles = (uint64_t*)(buffer + 4);

        mCreatedHandlesForSnapshotLoad.clear();
        mCreatedHandlesForSnapshotLoadIndex = 0;

        for (uint32_t i = 0; i < handleCount; ++i) {
            VKDGS_LOG("handle to load: 0x%llx", (unsigned long long)(uintptr_t)handles[i]);
            mCreatedHandlesForSnapshotLoad.push_back(handles[i]);
            consumed += 8;
        }

        return consumed;
    }

    void clearCreatedHandlesForSnapshotLoad() {
        mCreatedHandlesForSnapshotLoad.clear();
        mCreatedHandlesForSnapshotLoadIndex = 0;
    }

    VkResult on_vkEnumerateInstanceVersion(android::base::BumpPool* pool, uint32_t* pApiVersion) {
        if (m_vk->vkEnumerateInstanceVersion) {
            VkResult res = m_vk->vkEnumerateInstanceVersion(pApiVersion);

            if (*pApiVersion > kMaxSafeVersion) {
                *pApiVersion = kMaxSafeVersion;
            }

            return res;
        }
        *pApiVersion = kMinVersion;
        return VK_SUCCESS;
    }

    VkResult on_vkCreateInstance(android::base::BumpPool* pool,
                                 const VkInstanceCreateInfo* pCreateInfo,
                                 const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
        std::vector<const char*> finalExts = filteredInstanceExtensionNames(
            pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames);

        // Create higher version instance whenever it is possible.
        uint32_t apiVersion = VK_MAKE_VERSION(1, 0, 0);
        if (pCreateInfo->pApplicationInfo) {
            apiVersion = pCreateInfo->pApplicationInfo->apiVersion;
        }
        if (m_vk->vkEnumerateInstanceVersion) {
            uint32_t instanceVersion;
            VkResult result = m_vk->vkEnumerateInstanceVersion(&instanceVersion);
            if (result == VK_SUCCESS && instanceVersion >= VK_MAKE_VERSION(1, 1, 0)) {
                apiVersion = instanceVersion;
            }
        }

        VkInstanceCreateInfo createInfoFiltered;
        VkApplicationInfo appInfo = {};
        deepcopy_VkInstanceCreateInfo(pool, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, pCreateInfo,
                                      &createInfoFiltered);

        createInfoFiltered.enabledExtensionCount = static_cast<uint32_t>(finalExts.size());
        createInfoFiltered.ppEnabledExtensionNames = finalExts.data();
        if (createInfoFiltered.pApplicationInfo != nullptr) {
            const_cast<VkApplicationInfo*>(createInfoFiltered.pApplicationInfo)->apiVersion =
                apiVersion;
            appInfo = *createInfoFiltered.pApplicationInfo;
        }

        // remove VkDebugReportCallbackCreateInfoEXT and
        // VkDebugUtilsMessengerCreateInfoEXT from the chain.
        auto* curr = reinterpret_cast<vk_struct_common*>(&createInfoFiltered);
        while (curr != nullptr) {
            if (curr->pNext != nullptr &&
                (curr->pNext->sType == VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT ||
                 curr->pNext->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT)) {
                curr->pNext = curr->pNext->pNext;
            }
            curr = curr->pNext;
        }

#if defined(__APPLE__)
        if (m_emu->instanceSupportsMoltenVK) {
            createInfoFiltered.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

        // bug: 155795731
        bool swiftshader =
            (android::base::getEnvironmentVariable("ANDROID_EMU_VK_ICD").compare("swiftshader") ==
             0);
        std::unique_ptr<std::lock_guard<std::recursive_mutex>> lock = nullptr;

        if (swiftshader) {
            if (mLogging) {
                fprintf(stderr, "%s: acquire lock\n", __func__);
            }
            lock = std::make_unique<std::lock_guard<std::recursive_mutex>>(mLock);
        }

        VkResult res = m_vk->vkCreateInstance(&createInfoFiltered, pAllocator, pInstance);

        if (res != VK_SUCCESS) {
            WARN("Failed to create Vulkan instance: %s.", string_VkResult(res));
            return res;
        }

        if (!swiftshader) {
            lock = std::make_unique<std::lock_guard<std::recursive_mutex>>(mLock);
        }

        InstanceInfo info;
        info.apiVersion = apiVersion;
        if (pCreateInfo->pApplicationInfo) {
            if (pCreateInfo->pApplicationInfo->pApplicationName) {
                info.applicationName = pCreateInfo->pApplicationInfo->pApplicationName;
            }
            if (pCreateInfo->pApplicationInfo->pEngineName) {
                info.engineName = pCreateInfo->pApplicationInfo->pEngineName;
            }
        }
        for (uint32_t i = 0; i < createInfoFiltered.enabledExtensionCount; ++i) {
            info.enabledExtensionNames.push_back(createInfoFiltered.ppEnabledExtensionNames[i]);
        }

        INFO("Created VkInstance:%p for application:%s engine:%s.", *pInstance,
             info.applicationName.c_str(), info.engineName.c_str());

#ifdef GFXSTREAM_ENABLE_HOST_VK_SNAPSHOT
        // TODO: bug 129484301
        if (!m_emu->features.VulkanSnapshots.enabled ||
            (kSnapshotAppAllowList.find(info.applicationName) == kSnapshotAppAllowList.end() &&
             kSnapshotEngineAllowList.find(info.engineName) == kSnapshotEngineAllowList.end())) {
            get_emugl_vm_operations().setSkipSnapshotSave(true);
            get_emugl_vm_operations().setSkipSnapshotSaveReason(SNAPSHOT_SKIP_UNSUPPORTED_VK_APP);
        }
#endif
        // Box it up
        VkInstance boxed = new_boxed_VkInstance(*pInstance, nullptr, true /* own dispatch */);
        init_vulkan_dispatch_from_instance(m_vk, *pInstance, dispatch_VkInstance(boxed));
        info.boxed = boxed;

        std::string_view engineName = appInfo.pEngineName ? appInfo.pEngineName : "";
        info.isAngle = (engineName == "ANGLE");

        mInstanceInfo[*pInstance] = info;

        *pInstance = (VkInstance)info.boxed;

        if (vkCleanupEnabled()) {
            m_emu->callbacks.registerProcessCleanupCallback(unbox_VkInstance(boxed), [this, boxed] {
                if (snapshotsEnabled()) {
                    snapshot()->vkDestroyInstance(nullptr, 0, nullptr, boxed, nullptr);
                }
                vkDestroyInstanceImpl(unbox_VkInstance(boxed), nullptr);
            });
        }

        return res;
    }

    void vkDestroyInstanceImpl(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
        // Do delayed removes out of the lock, but get the list of devices to destroy inside the
        // lock.
        {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            std::vector<VkDevice> devicesToDestroy;

            for (auto it : mDeviceToPhysicalDevice) {
                auto* otherInstance = android::base::find(mPhysicalDeviceToInstance, it.second);
                if (!otherInstance) continue;
                if (instance == *otherInstance) {
                    devicesToDestroy.push_back(it.first);
                }
            }

            for (auto device : devicesToDestroy) {
                sBoxedHandleManager.processDelayedRemovesGlobalStateLocked(device);
            }
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);

        teardownInstanceLocked(instance);

        if (mRenderDocWithMultipleVkInstances) {
            mRenderDocWithMultipleVkInstances->removeVkInstance(instance);
        }
        m_vk->vkDestroyInstance(instance, pAllocator);

        auto it = mPhysicalDeviceToInstance.begin();

        while (it != mPhysicalDeviceToInstance.end()) {
            if (it->second == instance) {
                it = mPhysicalDeviceToInstance.erase(it);
            } else {
                ++it;
            }
        }

        auto* instInfo = android::base::find(mInstanceInfo, instance);
        delete_VkInstance(instInfo->boxed);
        mInstanceInfo.erase(instance);
    }

    void on_vkDestroyInstance(android::base::BumpPool* pool, VkInstance boxed_instance,
                              const VkAllocationCallbacks* pAllocator) {
        auto instance = unbox_VkInstance(boxed_instance);

        vkDestroyInstanceImpl(instance, pAllocator);

        m_emu->callbacks.unregisterProcessCleanupCallback(instance);
    }

    VkResult GetPhysicalDevices(VkInstance instance, VulkanDispatch* vk,
                                std::vector<VkPhysicalDevice>& outPhysicalDevices) {
        uint32_t physicalDevicesCount = 0;
        auto res = vk->vkEnumeratePhysicalDevices(instance, &physicalDevicesCount, nullptr);
        if (res != VK_SUCCESS) {
            return res;
        }

        outPhysicalDevices.resize(physicalDevicesCount);

        res = vk->vkEnumeratePhysicalDevices(instance, &physicalDevicesCount,
                                             outPhysicalDevices.data());
        if (res != VK_SUCCESS) {
            outPhysicalDevices.clear();
            return res;
        }

        outPhysicalDevices.resize(physicalDevicesCount);

        return VK_SUCCESS;
    }

    void FilterPhysicalDevicesLocked(VkInstance instance, VulkanDispatch* vk,
                                     std::vector<VkPhysicalDevice>& toFilterPhysicalDevices) {
        if (m_emu->instanceSupportsGetPhysicalDeviceProperties2) {
            PFN_vkGetPhysicalDeviceProperties2KHR getPhysdevProps2Func =
                vk_util::getVkInstanceProcAddrWithFallback<
                    vk_util::vk_fn_info::GetPhysicalDeviceProperties2>(
                    {
                        vk->vkGetInstanceProcAddr,
                        m_vk->vkGetInstanceProcAddr,
                    },
                    instance);

            if (getPhysdevProps2Func) {
                // Remove those devices whose UUIDs don't match the one in VkCommonOperations.
                toFilterPhysicalDevices.erase(
                    std::remove_if(toFilterPhysicalDevices.begin(), toFilterPhysicalDevices.end(),
                                   [getPhysdevProps2Func, this](VkPhysicalDevice physicalDevice) {
                                       // We can get the device UUID.
                                       VkPhysicalDeviceIDPropertiesKHR idProps = {
                                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR,
                                           nullptr,
                                       };
                                       VkPhysicalDeviceProperties2KHR propsWithId = {
                                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR,
                                           &idProps,
                                       };
                                       getPhysdevProps2Func(physicalDevice, &propsWithId);

                                       return memcmp(m_emu->deviceInfo.idProps.deviceUUID,
                                                     idProps.deviceUUID, VK_UUID_SIZE) != 0;
                                   }),
                    toFilterPhysicalDevices.end());
            } else {
                ERR("Failed to vkGetPhysicalDeviceProperties2KHR().");
            }
        } else {
            // If we don't support ID properties then just advertise only the
            // first physical device.
            WARN("Device ID not available, returning first physical device.");
        }
        if (!toFilterPhysicalDevices.empty()) {
            toFilterPhysicalDevices.erase(std::next(toFilterPhysicalDevices.begin()),
                                          toFilterPhysicalDevices.end());
        }
    }

    VkResult on_vkEnumeratePhysicalDevices(android::base::BumpPool* pool, VkInstance boxed_instance,
                                           uint32_t* pPhysicalDeviceCount,
                                           VkPhysicalDevice* pPhysicalDevices) {
        auto instance = unbox_VkInstance(boxed_instance);
        auto vk = dispatch_VkInstance(boxed_instance);

        std::vector<VkPhysicalDevice> physicalDevices;
        auto res = GetPhysicalDevices(instance, vk, physicalDevices);
        if (res != VK_SUCCESS) {
            return res;
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);

        FilterPhysicalDevicesLocked(instance, vk, physicalDevices);

        const uint32_t requestedCount = pPhysicalDeviceCount ? *pPhysicalDeviceCount : 0;
        const uint32_t availableCount = static_cast<uint32_t>(physicalDevices.size());

        if (pPhysicalDeviceCount) {
            *pPhysicalDeviceCount = availableCount;
        }

        if (pPhysicalDeviceCount && pPhysicalDevices) {
            // Box them up
            for (uint32_t i = 0; i < std::min(requestedCount, availableCount); ++i) {
                mPhysicalDeviceToInstance[physicalDevices[i]] = instance;

                auto& physdevInfo = mPhysdevInfo[physicalDevices[i]];
                physdevInfo.instance = instance;
                physdevInfo.boxed = new_boxed_VkPhysicalDevice(physicalDevices[i], vk,
                                                               false /* does not own dispatch */);

                vk->vkGetPhysicalDeviceProperties(physicalDevices[i], &physdevInfo.props);

                if (physdevInfo.props.apiVersion > kMaxSafeVersion) {
                    physdevInfo.props.apiVersion = kMaxSafeVersion;
                }

                VkPhysicalDeviceMemoryProperties hostMemoryProperties;
                vk->vkGetPhysicalDeviceMemoryProperties(physicalDevices[i], &hostMemoryProperties);

                physdevInfo.memoryPropertiesHelper =
                    std::make_unique<EmulatedPhysicalDeviceMemoryProperties>(
                        hostMemoryProperties,
                        m_emu->representativeColorBufferMemoryTypeInfo->hostMemoryTypeIndex,
                        getFeatures());

                uint32_t queueFamilyPropCount = 0;

                vk->vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i],
                                                             &queueFamilyPropCount, nullptr);

                physdevInfo.queueFamilyProperties.resize((size_t)queueFamilyPropCount);

                vk->vkGetPhysicalDeviceQueueFamilyProperties(
                    physicalDevices[i], &queueFamilyPropCount,
                    physdevInfo.queueFamilyProperties.data());

                pPhysicalDevices[i] = (VkPhysicalDevice)physdevInfo.boxed;
            }
            if (requestedCount < availableCount) {
                res = VK_INCOMPLETE;
            }
        }

        return res;
    }

    void on_vkGetPhysicalDeviceFeatures(android::base::BumpPool* pool,
                                        VkPhysicalDevice boxed_physicalDevice,
                                        VkPhysicalDeviceFeatures* pFeatures) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);
        auto vk = dispatch_VkPhysicalDevice(boxed_physicalDevice);

        vk->vkGetPhysicalDeviceFeatures(physicalDevice, pFeatures);
        pFeatures->textureCompressionETC2 |= enableEmulatedEtc2(physicalDevice, vk);
        pFeatures->textureCompressionASTC_LDR |= enableEmulatedAstc(physicalDevice, vk);
    }

    void on_vkGetPhysicalDeviceFeatures2(android::base::BumpPool* pool,
                                         VkPhysicalDevice boxed_physicalDevice,
                                         VkPhysicalDeviceFeatures2* pFeatures) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);
        auto vk = dispatch_VkPhysicalDevice(boxed_physicalDevice);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* physdevInfo = android::base::find(mPhysdevInfo, physicalDevice);
        if (!physdevInfo) return;

        auto instance = mPhysicalDeviceToInstance[physicalDevice];
        auto* instanceInfo = android::base::find(mInstanceInfo, instance);
        if (!instanceInfo) return;

        if (instanceInfo->apiVersion >= VK_MAKE_VERSION(1, 1, 0) &&
            physdevInfo->props.apiVersion >= VK_MAKE_VERSION(1, 1, 0)) {
            vk->vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
        } else if (hasInstanceExtension(instance,
                                        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            vk->vkGetPhysicalDeviceFeatures2KHR(physicalDevice, pFeatures);
        } else {
            // No instance extension, fake it!!!!
            if (pFeatures->pNext) {
                fprintf(stderr,
                        "%s: Warning: Trying to use extension struct in "
                        "VkPhysicalDeviceFeatures2 without having enabled "
                        "the extension!\n",
                        __func__);
            }
            *pFeatures = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                0,
            };
            vk->vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);
        }

        pFeatures->features.textureCompressionETC2 |= enableEmulatedEtc2(physicalDevice, vk);
        pFeatures->features.textureCompressionASTC_LDR |= enableEmulatedAstc(physicalDevice, vk);
        VkPhysicalDeviceSamplerYcbcrConversionFeatures* ycbcrFeatures =
            vk_find_struct<VkPhysicalDeviceSamplerYcbcrConversionFeatures>(pFeatures);
        if (ycbcrFeatures != nullptr) {
            ycbcrFeatures->samplerYcbcrConversion |= m_emu->enableYcbcrEmulation;
        }
        VkPhysicalDeviceProtectedMemoryFeatures* protectedMemoryFeatures =
            vk_find_struct<VkPhysicalDeviceProtectedMemoryFeatures>(pFeatures);
        if (protectedMemoryFeatures != nullptr) {
            // Protected memory is not supported on emulators. Override feature
            // information to mark as unsupported (see b/329845987).
            protectedMemoryFeatures->protectedMemory = VK_FALSE;
        }

        VkPhysicalDevicePrivateDataFeatures* privateDataFeatures =
            vk_find_struct<VkPhysicalDevicePrivateDataFeatures>(pFeatures);
        if (privateDataFeatures != nullptr) {
            // Private data from the guest side is not currently supported and causes emulator
            // crashes with the dEQP-VK.api.object_management.private_data tests (b/368009403).
            privateDataFeatures->privateData = VK_FALSE;
        }
    }

    VkResult on_vkGetPhysicalDeviceImageFormatProperties(
        android::base::BumpPool* pool, VkPhysicalDevice boxed_physicalDevice, VkFormat format,
        VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
        VkImageFormatProperties* pImageFormatProperties) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);
        auto vk = dispatch_VkPhysicalDevice(boxed_physicalDevice);
        const bool emulatedTexture = isEmulatedCompressedTexture(format, physicalDevice, vk);
        if (emulatedTexture) {
            if (!supportEmulatedCompressedImageFormatProperty(format, type, tiling, usage, flags)) {
                memset(pImageFormatProperties, 0, sizeof(VkImageFormatProperties));
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
            flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            format = CompressedImageInfo::getCompressedMipmapsFormat(format);
        }

        VkResult res = vk->vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice, format, type, tiling, usage, flags, pImageFormatProperties);
        if (res != VK_SUCCESS) {
            return res;
        }
        if (emulatedTexture) {
            maskImageFormatPropertiesForEmulatedTextures(pImageFormatProperties);
        }
        return res;
    }

    VkResult on_vkGetPhysicalDeviceImageFormatProperties2(
        android::base::BumpPool* pool, VkPhysicalDevice boxed_physicalDevice,
        const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
        VkImageFormatProperties2* pImageFormatProperties) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);
        auto vk = dispatch_VkPhysicalDevice(boxed_physicalDevice);
        VkPhysicalDeviceImageFormatInfo2 imageFormatInfo;
        VkFormat format = pImageFormatInfo->format;
        const bool emulatedTexture = isEmulatedCompressedTexture(format, physicalDevice, vk);
        if (emulatedTexture) {
            if (!supportEmulatedCompressedImageFormatProperty(
                    pImageFormatInfo->format, pImageFormatInfo->type, pImageFormatInfo->tiling,
                    pImageFormatInfo->usage, pImageFormatInfo->flags)) {
                memset(&pImageFormatProperties->imageFormatProperties, 0,
                       sizeof(VkImageFormatProperties));
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            imageFormatInfo = *pImageFormatInfo;
            pImageFormatInfo = &imageFormatInfo;
            imageFormatInfo.flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
            imageFormatInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            imageFormatInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            imageFormatInfo.format = CompressedImageInfo::getCompressedMipmapsFormat(format);
        }
        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* physdevInfo = android::base::find(mPhysdevInfo, physicalDevice);
        if (!physdevInfo) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        VkResult res = VK_ERROR_INITIALIZATION_FAILED;

        auto instance = mPhysicalDeviceToInstance[physicalDevice];
        auto* instanceInfo = android::base::find(mInstanceInfo, instance);
        if (!instanceInfo) {
            return res;
        }

        if (instanceInfo->apiVersion >= VK_MAKE_VERSION(1, 1, 0) &&
            physdevInfo->props.apiVersion >= VK_MAKE_VERSION(1, 1, 0)) {
            res = vk->vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, pImageFormatInfo,
                                                                pImageFormatProperties);
        } else if (hasInstanceExtension(instance,
                                        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            res = vk->vkGetPhysicalDeviceImageFormatProperties2KHR(physicalDevice, pImageFormatInfo,
                                                                   pImageFormatProperties);
        } else {
            // No instance extension, fake it!!!!
            if (pImageFormatProperties->pNext) {
                fprintf(stderr,
                        "%s: Warning: Trying to use extension struct in "
                        "VkPhysicalDeviceFeatures2 without having enabled "
                        "the extension!!!!11111\n",
                        __func__);
            }
            *pImageFormatProperties = {
                VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                0,
            };
            res = vk->vkGetPhysicalDeviceImageFormatProperties(
                physicalDevice, pImageFormatInfo->format, pImageFormatInfo->type,
                pImageFormatInfo->tiling, pImageFormatInfo->usage, pImageFormatInfo->flags,
                &pImageFormatProperties->imageFormatProperties);
        }
        if (res != VK_SUCCESS) {
            return res;
        }

        const VkPhysicalDeviceExternalImageFormatInfo* extImageFormatInfo =
            vk_find_struct<VkPhysicalDeviceExternalImageFormatInfo>(pImageFormatInfo);
        VkExternalImageFormatProperties* extImageFormatProps =
            vk_find_struct<VkExternalImageFormatProperties>(pImageFormatProperties);

        // Only allow dedicated allocations for external images.
        if (extImageFormatInfo && extImageFormatProps) {
            extImageFormatProps->externalMemoryProperties.externalMemoryFeatures |=
                VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
        }

        if (emulatedTexture) {
            maskImageFormatPropertiesForEmulatedTextures(
                &pImageFormatProperties->imageFormatProperties);
        }

        return res;
    }

    void on_vkGetPhysicalDeviceFormatProperties(android::base::BumpPool* pool,
                                                VkPhysicalDevice boxed_physicalDevice,
                                                VkFormat format,
                                                VkFormatProperties* pFormatProperties) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);
        auto vk = dispatch_VkPhysicalDevice(boxed_physicalDevice);
        getPhysicalDeviceFormatPropertiesCore<VkFormatProperties>(
            [vk](VkPhysicalDevice physicalDevice, VkFormat format,
                 VkFormatProperties* pFormatProperties) {
                vk->vkGetPhysicalDeviceFormatProperties(physicalDevice, format, pFormatProperties);
            },
            vk, physicalDevice, format, pFormatProperties);
    }

    void on_vkGetPhysicalDeviceFormatProperties2(android::base::BumpPool* pool,
                                                 VkPhysicalDevice boxed_physicalDevice,
                                                 VkFormat format,
                                                 VkFormatProperties2* pFormatProperties) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);
        auto vk = dispatch_VkPhysicalDevice(boxed_physicalDevice);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* physdevInfo = android::base::find(mPhysdevInfo, physicalDevice);
        if (!physdevInfo) return;

        auto instance = mPhysicalDeviceToInstance[physicalDevice];
        auto* instanceInfo = android::base::find(mInstanceInfo, instance);
        if (!instanceInfo) return;

        if (instanceInfo->apiVersion >= VK_MAKE_VERSION(1, 1, 0) &&
            physdevInfo->props.apiVersion >= VK_MAKE_VERSION(1, 1, 0)) {
            getPhysicalDeviceFormatPropertiesCore<VkFormatProperties2>(
                [vk](VkPhysicalDevice physicalDevice, VkFormat format,
                     VkFormatProperties2* pFormatProperties) {
                    vk->vkGetPhysicalDeviceFormatProperties2(physicalDevice, format,
                                                             pFormatProperties);
                },
                vk, physicalDevice, format, pFormatProperties);
        } else if (hasInstanceExtension(instance,
                                        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            getPhysicalDeviceFormatPropertiesCore<VkFormatProperties2>(
                [vk](VkPhysicalDevice physicalDevice, VkFormat format,
                     VkFormatProperties2* pFormatProperties) {
                    vk->vkGetPhysicalDeviceFormatProperties2KHR(physicalDevice, format,
                                                                pFormatProperties);
                },
                vk, physicalDevice, format, pFormatProperties);
        } else {
            // No instance extension, fake it!!!!
            if (pFormatProperties->pNext) {
                fprintf(stderr,
                        "%s: Warning: Trying to use extension struct in "
                        "vkGetPhysicalDeviceFormatProperties2 without having "
                        "enabled the extension!!!!11111\n",
                        __func__);
            }
            pFormatProperties->sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
            getPhysicalDeviceFormatPropertiesCore<VkFormatProperties>(
                [vk](VkPhysicalDevice physicalDevice, VkFormat format,
                     VkFormatProperties* pFormatProperties) {
                    vk->vkGetPhysicalDeviceFormatProperties(physicalDevice, format,
                                                            pFormatProperties);
                },
                vk, physicalDevice, format, &pFormatProperties->formatProperties);
        }
    }

    void on_vkGetPhysicalDeviceProperties(android::base::BumpPool* pool,
                                          VkPhysicalDevice boxed_physicalDevice,
                                          VkPhysicalDeviceProperties* pProperties) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);
        auto vk = dispatch_VkPhysicalDevice(boxed_physicalDevice);

        vk->vkGetPhysicalDeviceProperties(physicalDevice, pProperties);

        if (pProperties->apiVersion > kMaxSafeVersion) {
            pProperties->apiVersion = kMaxSafeVersion;
        }
    }

    void on_vkGetPhysicalDeviceProperties2(android::base::BumpPool* pool,
                                           VkPhysicalDevice boxed_physicalDevice,
                                           VkPhysicalDeviceProperties2* pProperties) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);
        auto vk = dispatch_VkPhysicalDevice(boxed_physicalDevice);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* physdevInfo = android::base::find(mPhysdevInfo, physicalDevice);
        if (!physdevInfo) return;

        auto instance = mPhysicalDeviceToInstance[physicalDevice];
        auto* instanceInfo = android::base::find(mInstanceInfo, instance);
        if (!instanceInfo) return;

        if (instanceInfo->apiVersion >= VK_MAKE_VERSION(1, 1, 0) &&
            physdevInfo->props.apiVersion >= VK_MAKE_VERSION(1, 1, 0)) {
            vk->vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
        } else if (hasInstanceExtension(instance,
                                        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            vk->vkGetPhysicalDeviceProperties2KHR(physicalDevice, pProperties);
        } else {
            // No instance extension, fake it!!!!
            if (pProperties->pNext) {
                fprintf(stderr,
                        "%s: Warning: Trying to use extension struct in "
                        "VkPhysicalDeviceProperties2 without having enabled "
                        "the extension!!!!11111\n",
                        __func__);
            }
            *pProperties = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                0,
            };
            vk->vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);
        }

        if (pProperties->properties.apiVersion > kMaxSafeVersion) {
            pProperties->properties.apiVersion = kMaxSafeVersion;
        }
    }

    void on_vkGetPhysicalDeviceMemoryProperties(
        android::base::BumpPool* pool, VkPhysicalDevice boxed_physicalDevice,
        VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);
        auto vk = dispatch_VkPhysicalDevice(boxed_physicalDevice);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* physicalDeviceInfo = android::base::find(mPhysdevInfo, physicalDevice);
        if (!physicalDeviceInfo) {
            ERR("Failed to find physical device info.");
            return;
        }

        auto& physicalDeviceMemoryHelper = physicalDeviceInfo->memoryPropertiesHelper;
        *pMemoryProperties = physicalDeviceMemoryHelper->getGuestMemoryProperties();
    }

    void on_vkGetPhysicalDeviceMemoryProperties2(
        android::base::BumpPool* pool, VkPhysicalDevice boxed_physicalDevice,
        VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);
        auto vk = dispatch_VkPhysicalDevice(boxed_physicalDevice);

        auto* physicalDeviceInfo = android::base::find(mPhysdevInfo, physicalDevice);
        if (!physicalDeviceInfo) return;

        auto instance = mPhysicalDeviceToInstance[physicalDevice];
        auto* instanceInfo = android::base::find(mInstanceInfo, instance);
        if (!instanceInfo) return;

        if (instanceInfo->apiVersion >= VK_MAKE_VERSION(1, 1, 0) &&
            physicalDeviceInfo->props.apiVersion >= VK_MAKE_VERSION(1, 1, 0)) {
            vk->vkGetPhysicalDeviceMemoryProperties2(physicalDevice, pMemoryProperties);
        } else if (hasInstanceExtension(instance,
                                        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            vk->vkGetPhysicalDeviceMemoryProperties2KHR(physicalDevice, pMemoryProperties);
        } else {
            // No instance extension, fake it!!!!
            if (pMemoryProperties->pNext) {
                fprintf(stderr,
                        "%s: Warning: Trying to use extension struct in "
                        "VkPhysicalDeviceMemoryProperties2 without having enabled "
                        "the extension!!!!11111\n",
                        __func__);
            }
            *pMemoryProperties = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                0,
            };
        }

        auto& physicalDeviceMemoryHelper = physicalDeviceInfo->memoryPropertiesHelper;
        pMemoryProperties->memoryProperties =
            physicalDeviceMemoryHelper->getGuestMemoryProperties();
    }

    VkResult on_vkEnumerateDeviceExtensionProperties(android::base::BumpPool* pool,
                                                     VkPhysicalDevice boxed_physicalDevice,
                                                     const char* pLayerName,
                                                     uint32_t* pPropertyCount,
                                                     VkExtensionProperties* pProperties) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);
        auto vk = dispatch_VkPhysicalDevice(boxed_physicalDevice);

        bool shouldPassthrough = !m_emu->enableYcbcrEmulation;
#if defined(__APPLE__)
        shouldPassthrough = shouldPassthrough && !m_emu->instanceSupportsMoltenVK;
#endif
        if (shouldPassthrough) {
            return vk->vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName,
                                                            pPropertyCount, pProperties);
        }

        // If MoltenVK is supported on host, we need to ensure that we include
        // VK_MVK_moltenvk extenstion in returned properties.
        std::vector<VkExtensionProperties> properties;
        VkResult result =
            enumerateDeviceExtensionProperties(vk, physicalDevice, pLayerName, properties);
        if (result != VK_SUCCESS) {
            return result;
        }

#if defined(__APPLE__) && defined(VK_MVK_moltenvk)
        // Guest will check for VK_MVK_moltenvk extension for enabling AHB support
        if (m_emu->instanceSupportsMoltenVK &&
            !hasDeviceExtension(properties, VK_MVK_MOLTENVK_EXTENSION_NAME)) {
            VkExtensionProperties mvk_props;
            strncpy(mvk_props.extensionName, VK_MVK_MOLTENVK_EXTENSION_NAME,
                    sizeof(mvk_props.extensionName));
            mvk_props.specVersion = VK_MVK_MOLTENVK_SPEC_VERSION;
            properties.push_back(mvk_props);
        }
#endif

        if (m_emu->enableYcbcrEmulation &&
            !hasDeviceExtension(properties, VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME)) {
            VkExtensionProperties ycbcr_props;
            strncpy(ycbcr_props.extensionName, VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
                    sizeof(ycbcr_props.extensionName));
            ycbcr_props.specVersion = VK_KHR_SAMPLER_YCBCR_CONVERSION_SPEC_VERSION;
            properties.push_back(ycbcr_props);
        }
        if (pProperties == nullptr) {
            *pPropertyCount = properties.size();
        } else {
            // return number of structures actually written to pProperties.
            *pPropertyCount = std::min((uint32_t)properties.size(), *pPropertyCount);
            memcpy(pProperties, properties.data(), *pPropertyCount * sizeof(VkExtensionProperties));
        }
        return *pPropertyCount < properties.size() ? VK_INCOMPLETE : VK_SUCCESS;
    }

    VkResult on_vkCreateDevice(android::base::BumpPool* pool, VkPhysicalDevice boxed_physicalDevice,
                               const VkDeviceCreateInfo* pCreateInfo,
                               const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);
        auto vk = dispatch_VkPhysicalDevice(boxed_physicalDevice);

        std::vector<const char*> updatedDeviceExtensions =
            filteredDeviceExtensionNames(vk, physicalDevice, pCreateInfo->enabledExtensionCount,
                                         pCreateInfo->ppEnabledExtensionNames);

        m_emu->deviceLostHelper.addNeededDeviceExtensions(&updatedDeviceExtensions);

        uint32_t supportedFenceHandleTypes = 0;
        uint32_t supportedBinarySemaphoreHandleTypes = 0;
        // Run the underlying API call, filtering extensions.
        VkDeviceCreateInfo createInfoFiltered = *pCreateInfo;
        // According to the spec, it seems that the application can use compressed texture formats
        // without enabling the feature when creating the VkDevice, as long as
        // vkGetPhysicalDeviceFormatProperties and vkGetPhysicalDeviceImageFormatProperties reports
        // support: to query for additional properties, or if the feature is not enabled,
        // vkGetPhysicalDeviceFormatProperties and vkGetPhysicalDeviceImageFormatProperties can be
        // used to check for supported properties of individual formats as normal.
        bool emulateTextureEtc2 = needEmulatedEtc2(physicalDevice, vk);
        bool emulateTextureAstc = needEmulatedAstc(physicalDevice, vk);
        VkPhysicalDeviceFeatures featuresFiltered;
        std::vector<VkPhysicalDeviceFeatures*> featuresToFilter;

        if (pCreateInfo->pEnabledFeatures) {
            featuresFiltered = *pCreateInfo->pEnabledFeatures;
            createInfoFiltered.pEnabledFeatures = &featuresFiltered;
            featuresToFilter.emplace_back(&featuresFiltered);
        }

        if (VkPhysicalDeviceFeatures2* features2 =
                vk_find_struct<VkPhysicalDeviceFeatures2>(&createInfoFiltered)) {
            featuresToFilter.emplace_back(&features2->features);
        }

        for (VkPhysicalDeviceFeatures* feature : featuresToFilter) {
            if (emulateTextureEtc2) {
                feature->textureCompressionETC2 = VK_FALSE;
            }
            if (emulateTextureAstc) {
                feature->textureCompressionASTC_LDR = VK_FALSE;
            }
        }

        if (auto* ycbcrFeatures = vk_find_struct<VkPhysicalDeviceSamplerYcbcrConversionFeatures>(
                &createInfoFiltered)) {
            if (m_emu->enableYcbcrEmulation && !m_emu->deviceInfo.supportsSamplerYcbcrConversion) {
                ycbcrFeatures->samplerYcbcrConversion = VK_FALSE;
            }
        }

        if (auto* swapchainMaintenance1Features =
                vk_find_struct<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT>(
                    &createInfoFiltered)) {
            if (!supportsSwapchainMaintenance1(physicalDevice, vk)) {
                swapchainMaintenance1Features->swapchainMaintenance1 = VK_FALSE;
            }
        }

#ifdef __APPLE__
#ifndef VK_ENABLE_BETA_EXTENSIONS
        // TODO(b/349066492): Update Vulkan headers, stringhelpers and compilation parameters
        // to use this directly from beta extensions and use regular chain append commands
        const VkStructureType VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR =
            (VkStructureType)1000163000;
#endif
        // Enable all portability features supported on the device
        VkPhysicalDevicePortabilitySubsetFeaturesKHR supportedPortabilityFeatures = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR, nullptr};
        if (m_emu->instanceSupportsMoltenVK) {
            VkPhysicalDeviceFeatures2 features2 = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                .pNext = &supportedPortabilityFeatures,
            };
            vk->vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

            if (mVerbosePrints) {
                fprintf(stderr,
                        "VERBOSE:%s: MoltenVK supportedPortabilityFeatures\n"
                        "constantAlphaColorBlendFactors = %d\n"
                        "events = %d\n"
                        "imageViewFormatReinterpretation = %d\n"
                        "imageViewFormatSwizzle = %d\n"
                        "imageView2DOn3DImage = %d\n"
                        "multisampleArrayImage = %d\n"
                        "mutableComparisonSamplers = %d\n"
                        "pointPolygons = %d\n"
                        "samplerMipLodBias = %d\n"
                        "separateStencilMaskRef = %d\n"
                        "shaderSampleRateInterpolationFunctions = %d\n"
                        "tessellationIsolines = %d\n"
                        "tessellationPointMode = %d\n"
                        "triangleFans = %d\n"
                        "vertexAttributeAccessBeyondStride = %d\n",
                        __func__, supportedPortabilityFeatures.constantAlphaColorBlendFactors,
                        supportedPortabilityFeatures.events,
                        supportedPortabilityFeatures.imageViewFormatReinterpretation,
                        supportedPortabilityFeatures.imageViewFormatSwizzle,
                        supportedPortabilityFeatures.imageView2DOn3DImage,
                        supportedPortabilityFeatures.multisampleArrayImage,
                        supportedPortabilityFeatures.mutableComparisonSamplers,
                        supportedPortabilityFeatures.pointPolygons,
                        supportedPortabilityFeatures.samplerMipLodBias,
                        supportedPortabilityFeatures.separateStencilMaskRef,
                        supportedPortabilityFeatures.shaderSampleRateInterpolationFunctions,
                        supportedPortabilityFeatures.tessellationIsolines,
                        supportedPortabilityFeatures.tessellationPointMode,
                        supportedPortabilityFeatures.triangleFans,
                        supportedPortabilityFeatures.vertexAttributeAccessBeyondStride);
            }

            // Insert into device create info chain
            supportedPortabilityFeatures.pNext = const_cast<void*>(createInfoFiltered.pNext);
            createInfoFiltered.pNext = &supportedPortabilityFeatures;
        }
#endif

        // Filter device memory report as callbacks can not be passed between guest and host.
        vk_struct_chain_filter<VkDeviceDeviceMemoryReportCreateInfoEXT>(&createInfoFiltered);

        // Filter device groups as they are effectively disabled.
        vk_struct_chain_filter<VkDeviceGroupDeviceCreateInfo>(&createInfoFiltered);

        createInfoFiltered.enabledExtensionCount = (uint32_t)updatedDeviceExtensions.size();
        createInfoFiltered.ppEnabledExtensionNames = updatedDeviceExtensions.data();

        // bug: 155795731
        bool swiftshader =
            (android::base::getEnvironmentVariable("ANDROID_EMU_VK_ICD").compare("swiftshader") ==
             0);

        std::unique_ptr<std::lock_guard<std::recursive_mutex>> lock = nullptr;

        if (swiftshader) {
            lock = std::make_unique<std::lock_guard<std::recursive_mutex>>(mLock);
        }

        VkResult result =
            vk->vkCreateDevice(physicalDevice, &createInfoFiltered, pAllocator, pDevice);

        if (result != VK_SUCCESS) return result;

        if (!swiftshader) {
            lock = std::make_unique<std::lock_guard<std::recursive_mutex>>(mLock);
        }

        mDeviceToPhysicalDevice[*pDevice] = physicalDevice;

        auto physicalDeviceInfoIt = mPhysdevInfo.find(physicalDevice);
        if (physicalDeviceInfoIt == mPhysdevInfo.end()) return VK_ERROR_INITIALIZATION_FAILED;
        auto& physicalDeviceInfo = physicalDeviceInfoIt->second;

        auto instanceInfoIt = mInstanceInfo.find(physicalDeviceInfo.instance);
        if (instanceInfoIt == mInstanceInfo.end()) return VK_ERROR_INITIALIZATION_FAILED;
        auto& instanceInfo = instanceInfoIt->second;

        // Fill out information about the logical device here.
        auto& deviceInfo = mDeviceInfo[*pDevice];
        deviceInfo.physicalDevice = physicalDevice;
        deviceInfo.emulateTextureEtc2 = emulateTextureEtc2;
        deviceInfo.emulateTextureAstc = emulateTextureAstc;
        deviceInfo.useAstcCpuDecompression =
            m_emu->astcLdrEmulationMode == AstcEmulationMode::Cpu &&
            AstcCpuDecompressor::get().available();
        deviceInfo.decompPipelines =
            std::make_unique<GpuDecompressionPipelineManager>(m_vk, *pDevice);
        getSupportedFenceHandleTypes(vk, physicalDevice, &supportedFenceHandleTypes);
        getSupportedSemaphoreHandleTypes(vk, physicalDevice, &supportedBinarySemaphoreHandleTypes);

        deviceInfo.externalFenceInfo.supportedFenceHandleTypes =
            static_cast<VkExternalFenceHandleTypeFlagBits>(supportedFenceHandleTypes);
        deviceInfo.externalFenceInfo.supportedBinarySemaphoreHandleTypes =
            static_cast<VkExternalSemaphoreHandleTypeFlagBits>(supportedBinarySemaphoreHandleTypes);

        INFO("Created VkDevice:%p for application:%s engine:%s ASTC emulation:%s CPU decoding:%s.",
             *pDevice, instanceInfo.applicationName.c_str(), instanceInfo.engineName.c_str(),
             deviceInfo.emulateTextureAstc ? "on" : "off",
             deviceInfo.useAstcCpuDecompression ? "on" : "off");

        for (uint32_t i = 0; i < createInfoFiltered.enabledExtensionCount; ++i) {
            deviceInfo.enabledExtensionNames.push_back(
                createInfoFiltered.ppEnabledExtensionNames[i]);
        }

        // First, get the dispatch table.
        VkDevice boxed = new_boxed_VkDevice(*pDevice, nullptr, true /* own dispatch */);

        if (mLogging) {
            fprintf(stderr, "%s: init vulkan dispatch from device\n", __func__);
        }

        VulkanDispatch* dispatch = dispatch_VkDevice(boxed);
        init_vulkan_dispatch_from_device(vk, *pDevice, dispatch);
        if (m_emu->debugUtilsAvailableAndRequested) {
            deviceInfo.debugUtilsHelper = DebugUtilsHelper::withUtilsEnabled(*pDevice, dispatch);
        }

        deviceInfo.externalFencePool =
            std::make_unique<ExternalFencePool<VulkanDispatch>>(dispatch, *pDevice);

        deviceInfo.deviceOpTracker = std::make_shared<DeviceOpTracker>(*pDevice, dispatch);

        if (mLogging) {
            fprintf(stderr, "%s: init vulkan dispatch from device (end)\n", __func__);
        }

        deviceInfo.boxed = boxed;

        // Next, get information about the queue families used by this device.
        std::unordered_map<uint32_t, uint32_t> queueFamilyIndexCounts;
        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
            const auto& queueCreateInfo = pCreateInfo->pQueueCreateInfos[i];
            // Check only queues created with flags = 0 in VkDeviceQueueCreateInfo.
            auto flags = queueCreateInfo.flags;
            if (flags) continue;
            uint32_t queueFamilyIndex = queueCreateInfo.queueFamilyIndex;
            uint32_t queueCount = queueCreateInfo.queueCount;
            queueFamilyIndexCounts[queueFamilyIndex] = queueCount;
        }

        std::vector<uint64_t> extraHandles;
        for (auto it : queueFamilyIndexCounts) {
            auto index = it.first;
            auto count = it.second;
            auto& queues = deviceInfo.queues[index];
            for (uint32_t i = 0; i < count; ++i) {
                VkQueue queueOut;

                if (mLogging) {
                    fprintf(stderr, "%s: get device queue (begin)\n", __func__);
                }

                vk->vkGetDeviceQueue(*pDevice, index, i, &queueOut);

                if (mLogging) {
                    fprintf(stderr, "%s: get device queue (end)\n", __func__);
                }
                queues.push_back(queueOut);
                mQueueInfo[queueOut].device = *pDevice;
                mQueueInfo[queueOut].queueFamilyIndex = index;

                auto boxed = new_boxed_VkQueue(queueOut, dispatch_VkDevice(deviceInfo.boxed),
                                               false /* does not own dispatch */);
                extraHandles.push_back((uint64_t)boxed);
                mQueueInfo[queueOut].boxed = boxed;
                mQueueInfo[queueOut].lock = new Lock;
            }
        }
        if (snapshotsEnabled()) {
            snapshot()->createExtraHandlesForNextApi(extraHandles.data(), extraHandles.size());
        }

        // Box the device.
        *pDevice = (VkDevice)deviceInfo.boxed;

        if (mLogging) {
            fprintf(stderr, "%s: (end)\n", __func__);
        }

        return VK_SUCCESS;
    }

    void on_vkGetDeviceQueue(android::base::BumpPool* pool, VkDevice boxed_device,
                             uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue) {
        auto device = unbox_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        *pQueue = VK_NULL_HANDLE;

        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return;

        const auto& queues = deviceInfo->queues;

        const auto* queueList = android::base::find(queues, queueFamilyIndex);
        if (!queueList) return;
        if (queueIndex >= queueList->size()) return;

        VkQueue unboxedQueue = (*queueList)[queueIndex];

        auto* queueInfo = android::base::find(mQueueInfo, unboxedQueue);
        if (!queueInfo) return;

        *pQueue = (VkQueue)queueInfo->boxed;
    }

    void on_vkGetDeviceQueue2(android::base::BumpPool* pool, VkDevice boxed_device,
                              const VkDeviceQueueInfo2* pQueueInfo, VkQueue* pQueue) {
        // Protected memory is not supported on emulators. So we should
        // not return any queue if a client requests a protected device
        // queue. See b/328436383.
        if (pQueueInfo->flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT) {
            *pQueue = VK_NULL_HANDLE;
            fprintf(stderr, "%s: Cannot get protected Vulkan device queue\n", __func__);
            return;
        }
        uint32_t queueFamilyIndex = pQueueInfo->queueFamilyIndex;
        uint32_t queueIndex = pQueueInfo->queueIndex;
        on_vkGetDeviceQueue(pool, boxed_device, queueFamilyIndex, queueIndex, pQueue);
    }

    void destroyDeviceLocked(VkDevice device, const VkAllocationCallbacks* pAllocator) {
        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return;

        deviceInfo->decompPipelines->clear();

        auto eraseIt = mQueueInfo.begin();
        for (; eraseIt != mQueueInfo.end();) {
            if (eraseIt->second.device == device) {
                delete eraseIt->second.lock;
                delete_VkQueue(eraseIt->second.boxed);
                eraseIt = mQueueInfo.erase(eraseIt);
            } else {
                ++eraseIt;
            }
        }

        VulkanDispatch* deviceDispatch = dispatch_VkDevice(deviceInfo->boxed);

        for (auto fence : findDeviceObjects(device, mFenceInfo)) {
            destroyFenceLocked(device, deviceDispatch, fence, nullptr, false);
        }

        // Should happen before destroying fences
        deviceInfo->deviceOpTracker->OnDestroyDevice();

        // Destroy pooled external fences
        auto deviceFences = deviceInfo->externalFencePool->popAll();
        for (auto fence : deviceFences) {
            deviceDispatch->vkDestroyFence(device, fence, pAllocator);
            mFenceInfo.erase(fence);
        }

        // Run the underlying API call.
        m_vk->vkDestroyDevice(device, pAllocator);

        delete_VkDevice(deviceInfo->boxed);
    }

    void on_vkDestroyDevice(android::base::BumpPool* pool, VkDevice boxed_device,
                            const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        sBoxedHandleManager.processDelayedRemovesGlobalStateLocked(device);
        destroyDeviceLocked(device, pAllocator);

        mDeviceInfo.erase(device);
        mDeviceToPhysicalDevice.erase(device);
    }

    VkResult on_vkCreateBuffer(android::base::BumpPool* pool, VkDevice boxed_device,
                               const VkBufferCreateInfo* pCreateInfo,
                               const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);
        VkBufferCreateInfo localCreateInfo;
        if (snapshotsEnabled()) {
            localCreateInfo = *pCreateInfo;
            // Add transfer src bit for potential device local memories.
            //
            // There are 3 ways to populate buffer content:
            //   a) use host coherent memory and memory mapping;
            //   b) use transfer_dst and vkcmdcopy* (for device local memories);
            //   c) use storage and compute shaders.
            //
            // (a) is covered by memory snapshot. (b) requires an extra vkCmdCopyBuffer
            // command on snapshot, thuse we need to add transfer_src for (b) so that
            // they could be loaded back on snapshot save. (c) is still future work.
            if (localCreateInfo.usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) {
                localCreateInfo.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            }
            pCreateInfo = &localCreateInfo;
        }

        VkExternalMemoryBufferCreateInfo externalCI = {
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        if (m_emu->features.VulkanAllocateHostMemory.enabled) {
            localCreateInfo = *pCreateInfo;
            // Hint that we 'may' use host allocation for this buffer. This will only be used for
            // host visible memory.
            externalCI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

            // Insert the new struct to the chain
            externalCI.pNext = localCreateInfo.pNext;
            localCreateInfo.pNext = &externalCI;

            pCreateInfo = &localCreateInfo;
        }

        VkResult result = vk->vkCreateBuffer(device, pCreateInfo, pAllocator, pBuffer);

        if (result == VK_SUCCESS) {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            auto& bufInfo = mBufferInfo[*pBuffer];
            bufInfo.device = device;
            bufInfo.usage = pCreateInfo->usage;
            bufInfo.size = pCreateInfo->size;
            *pBuffer = new_boxed_non_dispatchable_VkBuffer(*pBuffer);
        }

        return result;
    }

    void on_vkDestroyBuffer(android::base::BumpPool* pool, VkDevice boxed_device, VkBuffer buffer,
                            const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        vk->vkDestroyBuffer(device, buffer, pAllocator);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        mBufferInfo.erase(buffer);
    }

    void setBufferMemoryBindInfoLocked(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                       VkDeviceSize memoryOffset) {
        auto* bufferInfo = android::base::find(mBufferInfo, buffer);
        if (!bufferInfo) return;
        bufferInfo->memory = memory;
        bufferInfo->memoryOffset = memoryOffset;

        auto* memoryInfo = android::base::find(mMemoryInfo, memory);
        if (memoryInfo && memoryInfo->boundBuffer) {
            auto* deviceInfo = android::base::find(mDeviceInfo, device);
            if (deviceInfo) {
                deviceInfo->debugUtilsHelper.addDebugLabel(buffer, "Buffer:%d",
                                                           *memoryInfo->boundBuffer);
            }
        }
    }

    VkResult on_vkBindBufferMemory(android::base::BumpPool* pool, VkDevice boxed_device,
                                   VkBuffer buffer, VkDeviceMemory memory,
                                   VkDeviceSize memoryOffset) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        VALIDATE_REQUIRED_HANDLE(memory);
        VkResult result = vk->vkBindBufferMemory(device, buffer, memory, memoryOffset);

        if (result == VK_SUCCESS) {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            setBufferMemoryBindInfoLocked(device, buffer, memory, memoryOffset);
        }
        return result;
    }

    VkResult on_vkBindBufferMemory2(android::base::BumpPool* pool, VkDevice boxed_device,
                                    uint32_t bindInfoCount,
                                    const VkBindBufferMemoryInfo* pBindInfos) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        for (uint32_t i = 0; i < bindInfoCount; ++i) {
            VALIDATE_REQUIRED_HANDLE(pBindInfos[i].memory);
        }
        VkResult result = vk->vkBindBufferMemory2(device, bindInfoCount, pBindInfos);

        if (result == VK_SUCCESS) {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            for (uint32_t i = 0; i < bindInfoCount; ++i) {
                setBufferMemoryBindInfoLocked(device, pBindInfos[i].buffer, pBindInfos[i].memory,
                                              pBindInfos[i].memoryOffset);
            }
        }

        return result;
    }

    VkResult on_vkBindBufferMemory2KHR(android::base::BumpPool* pool, VkDevice boxed_device,
                                       uint32_t bindInfoCount,
                                       const VkBindBufferMemoryInfo* pBindInfos) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        for (uint32_t i = 0; i < bindInfoCount; ++i) {
            VALIDATE_REQUIRED_HANDLE(pBindInfos[i].memory);
        }
        VkResult result = vk->vkBindBufferMemory2KHR(device, bindInfoCount, pBindInfos);

        if (result == VK_SUCCESS) {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            for (uint32_t i = 0; i < bindInfoCount; ++i) {
                setBufferMemoryBindInfoLocked(device, pBindInfos[i].buffer, pBindInfos[i].memory,
                                              pBindInfos[i].memoryOffset);
            }
        }

        return result;
    }

    VkResult on_vkCreateImage(android::base::BumpPool* pool, VkDevice boxed_device,
                              const VkImageCreateInfo* pCreateInfo,
                              const VkAllocationCallbacks* pAllocator, VkImage* pImage,
                              bool boxImage = true) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        if (deviceInfo->imageFormats.find(pCreateInfo->format) == deviceInfo->imageFormats.end()) {
            VERBOSE("gfxstream_texture_format_manifest: %s [%d]", string_VkFormat(pCreateInfo->format), pCreateInfo->format);
            deviceInfo->imageFormats.insert(pCreateInfo->format);
        }

        const bool needDecompression = deviceInfo->needEmulatedDecompression(pCreateInfo->format);
        CompressedImageInfo cmpInfo =
            needDecompression
                ? CompressedImageInfo(device, *pCreateInfo, deviceInfo->decompPipelines.get())
                : CompressedImageInfo(device);
        VkImageCreateInfo decompInfo;
        if (needDecompression) {
            decompInfo = cmpInfo.getOutputCreateInfo(*pCreateInfo);
            pCreateInfo = &decompInfo;
        }

        std::unique_ptr<AndroidNativeBufferInfo> anbInfo = nullptr;
        const VkNativeBufferANDROID* nativeBufferANDROID =
            vk_find_struct<VkNativeBufferANDROID>(pCreateInfo);

#if defined(__APPLE__)
        VkExportMetalObjectCreateInfoEXT metalImageExportCI = {
            VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT, nullptr,
            VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT};

        // Add VkExportMetalObjectCreateInfoEXT on MoltenVK
        if (m_emu->instanceSupportsMoltenVK) {
            const VkExternalMemoryImageCreateInfo* externalMemCI =
                vk_find_struct<VkExternalMemoryImageCreateInfo>(pCreateInfo);
            if (externalMemCI) {
                // Insert metalImageExportCI to the chain
                metalImageExportCI.pNext = externalMemCI->pNext;
                const_cast<VkExternalMemoryImageCreateInfo*>(externalMemCI)->pNext =
                    &metalImageExportCI;
            }
        }
#endif

        VkResult createRes = VK_SUCCESS;

        if (nativeBufferANDROID) {
            auto* physicalDevice = android::base::find(mDeviceToPhysicalDevice, device);
            if (!physicalDevice) {
                return VK_ERROR_DEVICE_LOST;
            }

            auto* physicalDeviceInfo = android::base::find(mPhysdevInfo, *physicalDevice);
            if (!physicalDeviceInfo) {
                return VK_ERROR_DEVICE_LOST;
            }

            const VkPhysicalDeviceMemoryProperties& memoryProperties =
                physicalDeviceInfo->memoryPropertiesHelper->getHostMemoryProperties();

            anbInfo = std::make_unique<AndroidNativeBufferInfo>();
            createRes =
                prepareAndroidNativeBufferImage(vk, device, *pool, pCreateInfo, nativeBufferANDROID,
                                                pAllocator, &memoryProperties, anbInfo.get());
            if (createRes == VK_SUCCESS) {
                *pImage = anbInfo->image;
            }
        } else {
            createRes = vk->vkCreateImage(device, pCreateInfo, pAllocator, pImage);
        }

        if (createRes != VK_SUCCESS) return createRes;

        if (needDecompression) {
            cmpInfo.setOutputImage(*pImage);
            cmpInfo.createCompressedMipmapImages(vk, *pCreateInfo);

            if (cmpInfo.isAstc()) {
                if (deviceInfo->useAstcCpuDecompression) {
                    cmpInfo.initAstcCpuDecompression(m_vk, mDeviceInfo[device].physicalDevice);
                }
            }
        }

        auto& imageInfo = mImageInfo[*pImage];
        imageInfo.device = device;
        imageInfo.cmpInfo = std::move(cmpInfo);
        imageInfo.imageCreateInfoShallow = vk_make_orphan_copy(*pCreateInfo);
        imageInfo.layout = pCreateInfo->initialLayout;
        if (nativeBufferANDROID) imageInfo.anbInfo = std::move(anbInfo);

        if (boxImage) {
            *pImage = new_boxed_non_dispatchable_VkImage(*pImage);
        }
        return createRes;
    }

    void destroyImageLocked(VkDevice device, VulkanDispatch* deviceDispatch, VkImage image,
                            const VkAllocationCallbacks* pAllocator) {
        auto* imageInfo = android::base::find(mImageInfo, image);
        if (!imageInfo) return;

        if (!imageInfo->anbInfo) {
            imageInfo->cmpInfo.destroy(deviceDispatch);
            if (image != imageInfo->cmpInfo.outputImage()) {
                deviceDispatch->vkDestroyImage(device, image, pAllocator);
            }
        }
        mImageInfo.erase(image);
    }

    void on_vkDestroyImage(android::base::BumpPool* pool, VkDevice boxed_device, VkImage image,
                           const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        destroyImageLocked(device, deviceDispatch, image, pAllocator);
    }

    VkResult performBindImageMemoryDeferredAhb(android::base::BumpPool* pool,
                                               VkDevice boxed_device,
                                               const VkBindImageMemoryInfo* bimi) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        auto original_underlying_image = bimi->image;
        auto original_boxed_image = unboxed_to_boxed_non_dispatchable_VkImage(original_underlying_image);

        VkImageCreateInfo ici = {};
        {
            std::lock_guard<std::recursive_mutex> lock(mLock);

            auto* imageInfo = android::base::find(mImageInfo, original_underlying_image);
            if (!imageInfo) {
                ERR("Image for deferred AHB bind does not exist.");
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }

            ici = imageInfo->imageCreateInfoShallow;
        }

        ici.pNext = vk_find_struct<VkNativeBufferANDROID>(bimi);
        if (!ici.pNext) {
            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                << "Missing VkNativeBufferANDROID for deferred AHB bind.";
        }

        VkImage underlying_replacement_image = VK_NULL_HANDLE;
        VkResult result = on_vkCreateImage(pool, boxed_device, &ici, nullptr,
                                           &underlying_replacement_image, false);
        if (result != VK_SUCCESS) {
            ERR("Failed to create image for deferred AHB bind.");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        on_vkDestroyImage(pool, boxed_device, original_underlying_image, nullptr);

        {
            std::lock_guard<std::recursive_mutex> lock(mLock);

            set_boxed_non_dispatchable_VkImage(original_boxed_image, underlying_replacement_image);
            const_cast<VkBindImageMemoryInfo*>(bimi)->image = underlying_replacement_image;
            const_cast<VkBindImageMemoryInfo*>(bimi)->memory = nullptr;
        }

        return VK_SUCCESS;
    }

    VkResult performBindImageMemory(android::base::BumpPool* pool, VkDevice boxed_device,
                                    const VkBindImageMemoryInfo* bimi) {
        auto image = bimi->image;
        auto memory = bimi->memory;
        auto memoryOffset = bimi->memoryOffset;

        const auto* anb = vk_find_struct<VkNativeBufferANDROID>(bimi);
        if (memory == VK_NULL_HANDLE && anb != nullptr) {
            return performBindImageMemoryDeferredAhb(pool, boxed_device, bimi);
        }

        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        VALIDATE_REQUIRED_HANDLE(memory);
        VkResult result = vk->vkBindImageMemory(device, image, memory, memoryOffset);
        if (result != VK_SUCCESS) {
            return result;
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;

        auto* memoryInfo = android::base::find(mMemoryInfo, memory);
        if (!memoryInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;

        auto* imageInfo = android::base::find(mImageInfo, image);
        if (!imageInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        imageInfo->boundColorBuffer = memoryInfo->boundColorBuffer;
        if (imageInfo->boundColorBuffer) {
            deviceInfo->debugUtilsHelper.addDebugLabel(image, "ColorBuffer:%d",
                                                       *imageInfo->boundColorBuffer);
        }
        imageInfo->memory = memory;

        if (!deviceInfo->emulateTextureEtc2 && !deviceInfo->emulateTextureAstc) {
            return VK_SUCCESS;
        }

        CompressedImageInfo& cmpInfo = imageInfo->cmpInfo;
        if (!deviceInfo->needEmulatedDecompression(cmpInfo)) {
            return VK_SUCCESS;
        }
        return cmpInfo.bindCompressedMipmapsMemory(vk, memory, memoryOffset);
    }

    VkResult on_vkBindImageMemory(android::base::BumpPool* pool, VkDevice boxed_device,
                                  VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
        const VkBindImageMemoryInfo bimi = {
            .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
            .pNext = nullptr,
            .image = image,
            .memory = memory,
            .memoryOffset = memoryOffset,
        };
        return performBindImageMemory(pool, boxed_device, &bimi);
    }

    VkResult on_vkBindImageMemory2(android::base::BumpPool* pool, VkDevice boxed_device,
                                   uint32_t bindInfoCount,
                                   const VkBindImageMemoryInfo* pBindInfos) {
#ifdef GFXSTREAM_ENABLE_HOST_VK_SNAPSHOT
        if (bindInfoCount > 1 && snapshotsEnabled()) {
            if (mVerbosePrints) {
                fprintf(stderr,
                    "vkBindImageMemory2 with more than 1 bindInfoCount not supporting snapshot");
            }
            get_emugl_vm_operations().setSkipSnapshotSave(true);
            get_emugl_vm_operations().setSkipSnapshotSaveReason(SNAPSHOT_SKIP_UNSUPPORTED_VK_API);
        }
#endif

        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);
        bool needEmulation = false;

        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return VK_ERROR_UNKNOWN;

        for (uint32_t i = 0; i < bindInfoCount; i++) {
            auto* imageInfo = android::base::find(mImageInfo, pBindInfos[i].image);
            if (!imageInfo) return VK_ERROR_UNKNOWN;

            const auto* anb = vk_find_struct<VkNativeBufferANDROID>(&pBindInfos[i]);
            if (anb != nullptr) {
                needEmulation = true;
                break;
            }

            if (deviceInfo->needEmulatedDecompression(imageInfo->cmpInfo)) {
                needEmulation = true;
                break;
            }
        }

        if (needEmulation) {
            VkResult result;
            for (uint32_t i = 0; i < bindInfoCount; i++) {
                result = performBindImageMemory(pool, boxed_device, &pBindInfos[i]);
                if (result != VK_SUCCESS) return result;
            }

            return VK_SUCCESS;
        }

        VkResult result = vk->vkBindImageMemory2(device, bindInfoCount, pBindInfos);
        if (result != VK_SUCCESS) {
            return result;
        }

        if (deviceInfo->debugUtilsHelper.isEnabled()) {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            for (uint32_t i = 0; i < bindInfoCount; i++) {
                auto* memoryInfo = android::base::find(mMemoryInfo, pBindInfos[i].memory);
                if (!memoryInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;

                if (memoryInfo->boundColorBuffer) {
                    deviceInfo->debugUtilsHelper.addDebugLabel(
                        pBindInfos[i].image, "ColorBuffer:%d", *memoryInfo->boundColorBuffer);
                }
            }
        }

        return result;
    }

    VkResult on_vkCreateImageView(android::base::BumpPool* pool, VkDevice boxed_device,
                                  const VkImageViewCreateInfo* pCreateInfo,
                                  const VkAllocationCallbacks* pAllocator, VkImageView* pView) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);
        if (!pCreateInfo) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        auto* imageInfo = android::base::find(mImageInfo, pCreateInfo->image);
        if (!deviceInfo || !imageInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        VkImageViewCreateInfo createInfo;
        bool needEmulatedAlpha = false;
        if (deviceInfo->needEmulatedDecompression(pCreateInfo->format)) {
            if (imageInfo->cmpInfo.outputImage()) {
                createInfo = *pCreateInfo;
                createInfo.format = CompressedImageInfo::getOutputFormat(pCreateInfo->format);
                needEmulatedAlpha = CompressedImageInfo::needEmulatedAlpha(pCreateInfo->format);
                createInfo.image = imageInfo->cmpInfo.outputImage();
                pCreateInfo = &createInfo;
            }
        } else if (deviceInfo->needEmulatedDecompression(imageInfo->cmpInfo)) {
            // Image view on the compressed mipmaps
            createInfo = *pCreateInfo;
            createInfo.format =
                CompressedImageInfo::getCompressedMipmapsFormat(pCreateInfo->format);
            needEmulatedAlpha = false;
            createInfo.image =
                imageInfo->cmpInfo.compressedMipmap(pCreateInfo->subresourceRange.baseMipLevel);
            createInfo.subresourceRange.baseMipLevel = 0;
            pCreateInfo = &createInfo;
        }
        if (imageInfo->anbInfo && imageInfo->anbInfo->externallyBacked) {
            createInfo = *pCreateInfo;
            pCreateInfo = &createInfo;
        }

        VkResult result = vk->vkCreateImageView(device, pCreateInfo, pAllocator, pView);
        if (result != VK_SUCCESS) {
            return result;
        }

        auto& imageViewInfo = mImageViewInfo[*pView];
        imageViewInfo.device = device;
        imageViewInfo.needEmulatedAlpha = needEmulatedAlpha;
        imageViewInfo.boundColorBuffer = imageInfo->boundColorBuffer;
        if (imageViewInfo.boundColorBuffer) {
            deviceInfo->debugUtilsHelper.addDebugLabel(*pView, "ColorBuffer:%d",
                                                       *imageViewInfo.boundColorBuffer);
        }

        *pView = new_boxed_non_dispatchable_VkImageView(*pView);
        return result;
    }

    void on_vkDestroyImageView(android::base::BumpPool* pool, VkDevice boxed_device,
                               VkImageView imageView, const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        vk->vkDestroyImageView(device, imageView, pAllocator);
        std::lock_guard<std::recursive_mutex> lock(mLock);
        mImageViewInfo.erase(imageView);
    }

    VkResult on_vkCreateSampler(android::base::BumpPool* pool, VkDevice boxed_device,
                                const VkSamplerCreateInfo* pCreateInfo,
                                const VkAllocationCallbacks* pAllocator, VkSampler* pSampler) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);
        VkResult result = vk->vkCreateSampler(device, pCreateInfo, pAllocator, pSampler);
        if (result != VK_SUCCESS) {
            return result;
        }
        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto& samplerInfo = mSamplerInfo[*pSampler];
        samplerInfo.device = device;
        deepcopy_VkSamplerCreateInfo(pool, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                     pCreateInfo, &samplerInfo.createInfo);
        // We emulate RGB with RGBA for some compressed textures, which does not
        // handle translarent border correctly.
        samplerInfo.needEmulatedAlpha =
            (pCreateInfo->addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
             pCreateInfo->addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
             pCreateInfo->addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) &&
            (pCreateInfo->borderColor == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK ||
             pCreateInfo->borderColor == VK_BORDER_COLOR_INT_TRANSPARENT_BLACK ||
             pCreateInfo->borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT ||
             pCreateInfo->borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT);

        *pSampler = new_boxed_non_dispatchable_VkSampler(*pSampler);

        return result;
    }

    void destroySamplerLocked(VkDevice device, VulkanDispatch* deviceDispatch, VkSampler sampler,
                              const VkAllocationCallbacks* pAllocator) {
        deviceDispatch->vkDestroySampler(device, sampler, pAllocator);

        auto* samplerInfo = android::base::find(mSamplerInfo, sampler);
        if (!samplerInfo) return;

        if (samplerInfo->emulatedborderSampler != VK_NULL_HANDLE) {
            deviceDispatch->vkDestroySampler(device, samplerInfo->emulatedborderSampler, nullptr);
        }
        mSamplerInfo.erase(sampler);
    }

    void on_vkDestroySampler(android::base::BumpPool* pool, VkDevice boxed_device,
                             VkSampler sampler, const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        destroySamplerLocked(device, deviceDispatch, sampler, pAllocator);
    }

    VkResult exportSemaphore(
        VulkanDispatch* vk, VkDevice device, VkSemaphore semaphore, VK_EXT_SYNC_HANDLE* outHandle,
        std::optional<VkExternalSemaphoreHandleTypeFlagBits> handleType = std::nullopt) {
#if defined(_WIN32)
        VkSemaphoreGetWin32HandleInfoKHR getWin32 = {
            VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
            0,
            semaphore,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
        };

        return vk->vkGetSemaphoreWin32HandleKHR(device, &getWin32, outHandle);
#elif defined(__linux__)
        VkExternalSemaphoreHandleTypeFlagBits handleTypeBits =
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (handleType) {
            handleTypeBits = *handleType;
        }

        VkSemaphoreGetFdInfoKHR getFd = {
            VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            0,
            semaphore,
            handleTypeBits,
        };

        if (!hasDeviceExtension(device, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
            // Note: VK_KHR_external_semaphore_fd might be advertised in the guest,
            // because SYNC_FD handling is performed guest-side only. But still need
            // need to error out here when handling a non-sync, opaque FD.
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        return vk->vkGetSemaphoreFdKHR(device, &getFd, outHandle);
#else
        return VK_ERROR_OUT_OF_HOST_MEMORY;
#endif
    }

    VkResult on_vkCreateSemaphore(android::base::BumpPool* pool, VkDevice boxed_device,
                                  const VkSemaphoreCreateInfo* pCreateInfo,
                                  const VkAllocationCallbacks* pAllocator,
                                  VkSemaphore* pSemaphore) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        VkSemaphoreCreateInfo localCreateInfo = vk_make_orphan_copy(*pCreateInfo);
        vk_struct_chain_iterator structChainIter = vk_make_chain_iterator(&localCreateInfo);

        bool timelineSemaphore = false;

        VkSemaphoreTypeCreateInfoKHR localSemaphoreTypeCreateInfo;
        if (const VkSemaphoreTypeCreateInfoKHR* semaphoreTypeCiPtr =
                vk_find_struct<VkSemaphoreTypeCreateInfoKHR>(pCreateInfo);
            semaphoreTypeCiPtr) {
            localSemaphoreTypeCreateInfo = vk_make_orphan_copy(*semaphoreTypeCiPtr);
            vk_append_struct(&structChainIter, &localSemaphoreTypeCreateInfo);

            if (localSemaphoreTypeCreateInfo.semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
                timelineSemaphore = true;
            }
        }

        VkExportSemaphoreCreateInfoKHR localExportSemaphoreCi = {};

        /* Timeline semaphores are exportable:
         *
         * "Timeline semaphore specific external sharing capabilities can be queried using
         *  vkGetPhysicalDeviceExternalSemaphoreProperties by chaining the new
         *  VkSemaphoreTypeCreateInfoKHR structure to its pExternalSemaphoreInfo structure.
         *  This allows having a different set of external semaphore handle types supported
         *  for timeline semaphores vs. binary semaphores."
         *
         *  We just don't support this here since neither Android or Zink use this feature
         *  with timeline semaphores yet.
         */
        if (m_emu->features.VulkanExternalSync.enabled && !timelineSemaphore) {
            localExportSemaphoreCi.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
            localExportSemaphoreCi.pNext = nullptr;

            {
                std::lock_guard<std::recursive_mutex> lock(mLock);
                auto* deviceInfo = android::base::find(mDeviceInfo, device);

                if (!deviceInfo) {
                    return VK_ERROR_DEVICE_LOST;
                }

                if (deviceInfo->externalFenceInfo.supportedBinarySemaphoreHandleTypes &
                    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT) {
                    localExportSemaphoreCi.handleTypes =
                        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
                } else if (deviceInfo->externalFenceInfo.supportedBinarySemaphoreHandleTypes &
                           VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT) {
                    localExportSemaphoreCi.handleTypes =
                        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
                } else if (deviceInfo->externalFenceInfo.supportedBinarySemaphoreHandleTypes &
                           VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
                    localExportSemaphoreCi.handleTypes =
                        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
                }
            }

            vk_append_struct(&structChainIter, &localExportSemaphoreCi);
        }

        VkResult res = vk->vkCreateSemaphore(device, &localCreateInfo, pAllocator, pSemaphore);

        if (res != VK_SUCCESS) return res;

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto& semaphoreInfo = mSemaphoreInfo[*pSemaphore];
        semaphoreInfo.device = device;

        *pSemaphore = new_boxed_non_dispatchable_VkSemaphore(*pSemaphore);

        return res;
    }

    VkResult on_vkCreateFence(android::base::BumpPool* pool, VkDevice boxed_device,
                              const VkFenceCreateInfo* pCreateInfo,
                              const VkAllocationCallbacks* pAllocator, VkFence* pFence) {
        VkFenceCreateInfo localCreateInfo;
        if (mSnapshotState == SnapshotState::Loading) {
            // On snapshot load we create all fences as signaled then reset those that are not.
            localCreateInfo = *pCreateInfo;
            pCreateInfo = &localCreateInfo;
            localCreateInfo.flags |= VK_FENCE_CREATE_SIGNALED_BIT;
        }
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        VkFenceCreateInfo& createInfo = const_cast<VkFenceCreateInfo&>(*pCreateInfo);

        const VkExportFenceCreateInfo* exportFenceInfoPtr =
            vk_find_struct<VkExportFenceCreateInfo>(pCreateInfo);
        bool exportSyncFd = exportFenceInfoPtr && (exportFenceInfoPtr->handleTypes &
                                                   VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT);
        bool fenceReused = false;

        *pFence = VK_NULL_HANDLE;

        if (exportSyncFd) {
            // Remove VkExportFenceCreateInfo, since host doesn't need to create
            // an exportable fence in this case
            ExternalFencePool<VulkanDispatch>* externalFencePool = nullptr;
            vk_struct_chain_remove(exportFenceInfoPtr, &createInfo);
            {
                std::lock_guard<std::recursive_mutex> lock(mLock);
                auto* deviceInfo = android::base::find(mDeviceInfo, device);
                if (!deviceInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
                externalFencePool = deviceInfo->externalFencePool.get();
            }
            *pFence = externalFencePool->pop(pCreateInfo);
            if (*pFence != VK_NULL_HANDLE) {
                fenceReused = true;
            }
        }

        if (*pFence == VK_NULL_HANDLE) {
            VkResult res = vk->vkCreateFence(device, &createInfo, pAllocator, pFence);
            if (res != VK_SUCCESS) {
                return res;
            }
        }

        {
            std::lock_guard<std::recursive_mutex> lock(mLock);

            DCHECK(fenceReused || mFenceInfo.find(*pFence) == mFenceInfo.end());
            // Create FenceInfo for *pFence.
            auto& fenceInfo = mFenceInfo[*pFence];
            fenceInfo.device = device;
            fenceInfo.vk = vk;

            *pFence = new_boxed_non_dispatchable_VkFence(*pFence);
            fenceInfo.boxed = *pFence;
            fenceInfo.external = exportSyncFd;
            fenceInfo.state = FenceInfo::State::kNotWaitable;
        }

        return VK_SUCCESS;
    }

    VkResult on_vkResetFences(android::base::BumpPool* pool, VkDevice boxed_device,
                              uint32_t fenceCount, const VkFence* pFences) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        std::vector<VkFence> cleanedFences;
        std::vector<VkFence> externalFences;

        {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            for (uint32_t i = 0; i < fenceCount; i++) {
                if (pFences[i] == VK_NULL_HANDLE) continue;

                DCHECK(mFenceInfo.find(pFences[i]) != mFenceInfo.end());
                if (mFenceInfo[pFences[i]].external) {
                    externalFences.push_back(pFences[i]);
                } else {
                    // Reset all fences' states to kNotWaitable.
                    cleanedFences.push_back(pFences[i]);
                    mFenceInfo[pFences[i]].state = FenceInfo::State::kNotWaitable;
                }
            }
        }

        if (!cleanedFences.empty()) {
            VK_CHECK(vk->vkResetFences(device, (uint32_t)cleanedFences.size(),
                                       cleanedFences.data()));
        }

        // For external fences, we unilaterally put them in the pool to ensure they finish
        // TODO: should store creation info / pNext chain per fence and re-apply?
        VkFenceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = 0, .flags = 0};
        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        for (auto fence : externalFences) {
            VkFence replacement = deviceInfo->externalFencePool->pop(&createInfo);
            if (replacement == VK_NULL_HANDLE) {
                VK_CHECK(vk->vkCreateFence(device, &createInfo, 0, &replacement));
            }
            deviceInfo->externalFencePool->add(fence);

            {
                std::lock_guard<std::recursive_mutex> lock(mLock);
                auto boxed_fence = unboxed_to_boxed_non_dispatchable_VkFence(fence);
                set_boxed_non_dispatchable_VkFence(boxed_fence, replacement);

                auto& fenceInfo = mFenceInfo[replacement];
                fenceInfo.device = device;
                fenceInfo.vk = vk;
                fenceInfo.boxed = boxed_fence;
                fenceInfo.external = true;
                fenceInfo.state = FenceInfo::State::kNotWaitable;

                mFenceInfo[fence].boxed = VK_NULL_HANDLE;
            }
        }

        return VK_SUCCESS;
    }

    VkResult on_vkImportSemaphoreFdKHR(android::base::BumpPool* pool, VkDevice boxed_device,
                                       const VkImportSemaphoreFdInfoKHR* pImportSemaphoreFdInfo) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

#ifdef _WIN32
        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* infoPtr = android::base::find(mSemaphoreInfo,
                                            mExternalSemaphoresById[pImportSemaphoreFdInfo->fd]);

        if (!infoPtr) {
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }

        VK_EXT_SYNC_HANDLE handle = dupExternalSync(infoPtr->externalHandle);

        VkImportSemaphoreWin32HandleInfoKHR win32ImportInfo = {
            VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
            0,
            pImportSemaphoreFdInfo->semaphore,
            pImportSemaphoreFdInfo->flags,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR,
            handle,
            L"",
        };

        return vk->vkImportSemaphoreWin32HandleKHR(device, &win32ImportInfo);
#else
        if (!hasDeviceExtension(device, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
            // Note: VK_KHR_external_semaphore_fd might be advertised in the guest,
            // because SYNC_FD handling is performed guest-side only. But still need
            // need to error out here when handling a non-sync, opaque FD.
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        VkImportSemaphoreFdInfoKHR importInfo = *pImportSemaphoreFdInfo;
        importInfo.fd = dup(pImportSemaphoreFdInfo->fd);
        return vk->vkImportSemaphoreFdKHR(device, &importInfo);
#endif
    }

    VkResult on_vkGetSemaphoreFdKHR(android::base::BumpPool* pool, VkDevice boxed_device,
                                    const VkSemaphoreGetFdInfoKHR* pGetFdInfo, int* pFd) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);
        VK_EXT_SYNC_HANDLE handle;

        VkResult result = exportSemaphore(vk, device, pGetFdInfo->semaphore, &handle);
        if (result != VK_SUCCESS) {
            return result;
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);
        mSemaphoreInfo[pGetFdInfo->semaphore].externalHandle = handle;
#ifdef _WIN32
        int nextId = genSemaphoreId();
        mExternalSemaphoresById[nextId] = pGetFdInfo->semaphore;
        *pFd = nextId;
#else
        // No next id; its already an fd
        mSemaphoreInfo[pGetFdInfo->semaphore].externalHandle = handle;
#endif
        return result;
    }

    VkResult on_vkGetSemaphoreGOOGLE(android::base::BumpPool* pool, VkDevice boxed_device,
                                     VkSemaphore semaphore, uint64_t syncId) {
        VK_EXT_SYNC_HANDLE handle;
        uint32_t streamHandleType = 0;
        auto* tInfo = RenderThreadInfoVk::get();
        auto vk = dispatch_VkDevice(boxed_device);
        auto device = unbox_VkDevice(boxed_device);
        VkExternalSemaphoreHandleTypeFlagBits flagBits =
            static_cast<VkExternalSemaphoreHandleTypeFlagBits>(0);

        if (!m_emu->features.VulkanExternalSync.enabled) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }

        {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            auto* deviceInfo = android::base::find(mDeviceInfo, device);

            if (!deviceInfo) {
                return VK_ERROR_DEVICE_LOST;
            }

            if (deviceInfo->externalFenceInfo.supportedBinarySemaphoreHandleTypes &
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT) {
                flagBits = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            } else if (deviceInfo->externalFenceInfo.supportedBinarySemaphoreHandleTypes &
                       VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT) {
                flagBits = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            } else if (deviceInfo->externalFenceInfo.supportedBinarySemaphoreHandleTypes &
                       VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
                flagBits = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            }
        }

        VkResult result =
            exportSemaphore(vk, device, semaphore, &handle,
                            std::make_optional<VkExternalSemaphoreHandleTypeFlagBits>(flagBits));
        if (result != VK_SUCCESS) {
            return result;
        }

        ManagedDescriptor descriptor(handle);
        ExternalObjectManager::get()->addSyncDescriptorInfo(
            tInfo->ctx_id, syncId, std::move(descriptor), streamHandleType);
        return VK_SUCCESS;
    }

    void destroySemaphoreLocked(VkDevice device, VulkanDispatch* deviceDispatch,
                                VkSemaphore semaphore, const VkAllocationCallbacks* pAllocator) {
        auto semaphoreInfoIt = mSemaphoreInfo.find(semaphore);
        if (semaphoreInfoIt == mSemaphoreInfo.end()) return;
        auto& semaphoreInfo = semaphoreInfoIt->second;

#ifndef _WIN32
        if (semaphoreInfo.externalHandle != VK_EXT_SYNC_HANDLE_INVALID) {
            close(semaphoreInfo.externalHandle);
        }
#endif

        if (semaphoreInfo.latestUse && !IsDone(*semaphoreInfo.latestUse)) {
            auto deviceInfoIt = mDeviceInfo.find(device);
            if (deviceInfoIt != mDeviceInfo.end()) {
                auto& deviceInfo = deviceInfoIt->second;
                deviceInfo.deviceOpTracker->AddPendingGarbage(*semaphoreInfo.latestUse, semaphore);
                deviceInfo.deviceOpTracker->PollAndProcessGarbage();
            }
        } else {
            deviceDispatch->vkDestroySemaphore(device, semaphore, pAllocator);
        }

        mSemaphoreInfo.erase(semaphoreInfoIt);
    }

    void on_vkDestroySemaphore(android::base::BumpPool* pool, VkDevice boxed_device,
                               VkSemaphore semaphore, const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        destroySemaphoreLocked(device, deviceDispatch, semaphore, pAllocator);
    }

    void destroyFenceLocked(VkDevice device, VulkanDispatch* deviceDispatch, VkFence fence,
                            const VkAllocationCallbacks* pAllocator,
                            bool allowExternalFenceRecycling) {
        if (VK_NULL_HANDLE == fence) {
            return;
        }

        auto fenceInfoIt = mFenceInfo.find(fence);
        if (fenceInfoIt == mFenceInfo.end()) {
            ERR("Failed to find fence info for VkFence:%p. Leaking fence!", fence);
            return;
        }
        auto& fenceInfo = fenceInfoIt->second;

        auto deviceInfoIt = mDeviceInfo.find(device);
        if (deviceInfoIt == mDeviceInfo.end()) {
            ERR("Failed to find device info for VkDevice:%p for VkFence:%p. Leaking fence!", device,
                fence);
            return;
        }
        auto& deviceInfo = deviceInfoIt->second;

        fenceInfo.boxed = VK_NULL_HANDLE;

        // External fences are just slated for recycling. This addresses known
        // behavior where the guest might destroy the fence prematurely. b/228221208
        if (fenceInfo.external) {
            if (allowExternalFenceRecycling) {
                deviceInfo.externalFencePool->add(fence);
            }
            return;
        }

        if (fenceInfo.latestUse && !IsDone(*fenceInfo.latestUse)) {
            deviceInfo.deviceOpTracker->AddPendingGarbage(*fenceInfo.latestUse, fence);
            deviceInfo.deviceOpTracker->PollAndProcessGarbage();
        } else {
            deviceDispatch->vkDestroyFence(device, fence, pAllocator);
        }

        mFenceInfo.erase(fenceInfoIt);
    }

    void on_vkDestroyFence(android::base::BumpPool* pool, VkDevice boxed_device, VkFence fence,
                           const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        destroyFenceLocked(device, deviceDispatch, fence, pAllocator, true);
    }

    VkResult on_vkCreateDescriptorSetLayout(android::base::BumpPool* pool, VkDevice boxed_device,
                                            const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                                            const VkAllocationCallbacks* pAllocator,
                                            VkDescriptorSetLayout* pSetLayout) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        auto res = vk->vkCreateDescriptorSetLayout(device, pCreateInfo, pAllocator, pSetLayout);

        if (res == VK_SUCCESS) {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            auto& info = mDescriptorSetLayoutInfo[*pSetLayout];
            info.device = device;
            *pSetLayout = new_boxed_non_dispatchable_VkDescriptorSetLayout(*pSetLayout);
            info.boxed = *pSetLayout;

            info.createInfo = *pCreateInfo;
            for (uint32_t i = 0; i < pCreateInfo->bindingCount; ++i) {
                info.bindings.push_back(pCreateInfo->pBindings[i]);
            }
        }

        return res;
    }

    void on_vkDestroyDescriptorSetLayout(android::base::BumpPool* pool, VkDevice boxed_device,
                                         VkDescriptorSetLayout descriptorSetLayout,
                                         const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        vk->vkDestroyDescriptorSetLayout(device, descriptorSetLayout, pAllocator);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        mDescriptorSetLayoutInfo.erase(descriptorSetLayout);
    }

    VkResult on_vkCreateDescriptorPool(android::base::BumpPool* pool, VkDevice boxed_device,
                                       const VkDescriptorPoolCreateInfo* pCreateInfo,
                                       const VkAllocationCallbacks* pAllocator,
                                       VkDescriptorPool* pDescriptorPool) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        auto res = vk->vkCreateDescriptorPool(device, pCreateInfo, pAllocator, pDescriptorPool);

        if (res == VK_SUCCESS) {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            auto& info = mDescriptorPoolInfo[*pDescriptorPool];
            info.device = device;
            *pDescriptorPool = new_boxed_non_dispatchable_VkDescriptorPool(*pDescriptorPool);
            info.boxed = *pDescriptorPool;
            info.createInfo = *pCreateInfo;
            info.maxSets = pCreateInfo->maxSets;
            info.usedSets = 0;

            for (uint32_t i = 0; i < pCreateInfo->poolSizeCount; ++i) {
                DescriptorPoolInfo::PoolState state;
                state.type = pCreateInfo->pPoolSizes[i].type;
                state.descriptorCount = pCreateInfo->pPoolSizes[i].descriptorCount;
                state.used = 0;
                info.pools.push_back(state);
            }

            if (m_emu->features.VulkanBatchedDescriptorSetUpdate.enabled) {
                for (uint32_t i = 0; i < pCreateInfo->maxSets; ++i) {
                    info.poolIds.push_back(
                        (uint64_t)new_boxed_non_dispatchable_VkDescriptorSet(VK_NULL_HANDLE));
                }
                if (snapshotsEnabled()) {
                    snapshot()->createExtraHandlesForNextApi(info.poolIds.data(),
                                                             info.poolIds.size());
                }
            }
        }

        return res;
    }

    void cleanupDescriptorPoolAllocedSetsLocked(VkDescriptorPool descriptorPool,
                                                bool isDestroy = false) {
        auto* info = android::base::find(mDescriptorPoolInfo, descriptorPool);
        if (!info) return;

        for (auto it : info->allocedSetsToBoxed) {
            auto unboxedSet = it.first;
            auto boxedSet = it.second;
            mDescriptorSetInfo.erase(unboxedSet);
            if (!m_emu->features.VulkanBatchedDescriptorSetUpdate.enabled) {
                delete_VkDescriptorSet(boxedSet);
            }
        }

        if (m_emu->features.VulkanBatchedDescriptorSetUpdate.enabled) {
            if (isDestroy) {
                for (auto poolId : info->poolIds) {
                    delete_VkDescriptorSet((VkDescriptorSet)poolId);
                }
            } else {
                for (auto poolId : info->poolIds) {
                    auto handleInfo = sBoxedHandleManager.get(poolId);
                    if (handleInfo)
                        handleInfo->underlying = reinterpret_cast<uint64_t>(VK_NULL_HANDLE);
                }
            }
        }

        info->usedSets = 0;
        info->allocedSetsToBoxed.clear();

        for (auto& pool : info->pools) {
            pool.used = 0;
        }
    }

    void on_vkDestroyDescriptorPool(android::base::BumpPool* pool, VkDevice boxed_device,
                                    VkDescriptorPool descriptorPool,
                                    const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        vk->vkDestroyDescriptorPool(device, descriptorPool, pAllocator);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        cleanupDescriptorPoolAllocedSetsLocked(descriptorPool, true /* destroy */);
        mDescriptorPoolInfo.erase(descriptorPool);
    }

    VkResult on_vkResetDescriptorPool(android::base::BumpPool* pool, VkDevice boxed_device,
                                      VkDescriptorPool descriptorPool,
                                      VkDescriptorPoolResetFlags flags) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        auto res = vk->vkResetDescriptorPool(device, descriptorPool, flags);

        if (res == VK_SUCCESS) {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            cleanupDescriptorPoolAllocedSetsLocked(descriptorPool);
        }

        return res;
    }

    void initDescriptorSetInfoLocked(VkDescriptorPool pool, VkDescriptorSetLayout setLayout,
                                     uint64_t boxedDescriptorSet, VkDescriptorSet descriptorSet) {
        auto* poolInfo = android::base::find(mDescriptorPoolInfo, pool);
        if (!poolInfo) {
            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Cannot find poolInfo";
        }

        auto* setLayoutInfo = android::base::find(mDescriptorSetLayoutInfo, setLayout);
        if (!setLayoutInfo) {
            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Cannot find setLayout";
        }

        auto& setInfo = mDescriptorSetInfo[descriptorSet];

        setInfo.pool = pool;
        setInfo.unboxedLayout = setLayout;
        setInfo.bindings = setLayoutInfo->bindings;
        for (size_t i = 0; i < setInfo.bindings.size(); i++) {
            VkDescriptorSetLayoutBinding dslBinding = setInfo.bindings[i];
            int bindingIdx = dslBinding.binding;
            if (setInfo.allWrites.size() <= bindingIdx) {
                setInfo.allWrites.resize(bindingIdx + 1);
            }
            setInfo.allWrites[bindingIdx].resize(dslBinding.descriptorCount);
            for (auto& write : setInfo.allWrites[bindingIdx]) {
                write.descriptorType = dslBinding.descriptorType;
                write.dstArrayElement = 0;
            }
        }

        poolInfo->allocedSetsToBoxed[descriptorSet] = (VkDescriptorSet)boxedDescriptorSet;
        applyDescriptorSetAllocationLocked(*poolInfo, setInfo.bindings);
    }

    VkResult on_vkAllocateDescriptorSets(android::base::BumpPool* pool, VkDevice boxed_device,
                                         const VkDescriptorSetAllocateInfo* pAllocateInfo,
                                         VkDescriptorSet* pDescriptorSets) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto allocValidationRes = validateDescriptorSetAllocLocked(pAllocateInfo);
        if (allocValidationRes != VK_SUCCESS) return allocValidationRes;

        auto res = vk->vkAllocateDescriptorSets(device, pAllocateInfo, pDescriptorSets);

        if (res == VK_SUCCESS) {
            auto* poolInfo =
                android::base::find(mDescriptorPoolInfo, pAllocateInfo->descriptorPool);
            if (!poolInfo) return res;

            for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; ++i) {
                auto unboxed = pDescriptorSets[i];
                pDescriptorSets[i] = new_boxed_non_dispatchable_VkDescriptorSet(pDescriptorSets[i]);
                initDescriptorSetInfoLocked(pAllocateInfo->descriptorPool,
                                            pAllocateInfo->pSetLayouts[i],
                                            (uint64_t)(pDescriptorSets[i]), unboxed);
            }
        }

        return res;
    }

    VkResult on_vkFreeDescriptorSets(android::base::BumpPool* pool, VkDevice boxed_device,
                                     VkDescriptorPool descriptorPool, uint32_t descriptorSetCount,
                                     const VkDescriptorSet* pDescriptorSets) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        auto res =
            vk->vkFreeDescriptorSets(device, descriptorPool, descriptorSetCount, pDescriptorSets);

        if (res == VK_SUCCESS) {
            std::lock_guard<std::recursive_mutex> lock(mLock);

            for (uint32_t i = 0; i < descriptorSetCount; ++i) {
                auto* setInfo = android::base::find(mDescriptorSetInfo, pDescriptorSets[i]);
                if (!setInfo) continue;
                auto* poolInfo = android::base::find(mDescriptorPoolInfo, setInfo->pool);
                if (!poolInfo) continue;

                removeDescriptorSetAllocationLocked(*poolInfo, setInfo->bindings);

                auto descSetAllocedEntry =
                    android::base::find(poolInfo->allocedSetsToBoxed, pDescriptorSets[i]);
                if (!descSetAllocedEntry) continue;

                auto handleInfo = sBoxedHandleManager.get((uint64_t)*descSetAllocedEntry);
                if (handleInfo) {
                    if (m_emu->features.VulkanBatchedDescriptorSetUpdate.enabled) {
                        handleInfo->underlying = reinterpret_cast<uint64_t>(VK_NULL_HANDLE);
                    } else {
                        delete_VkDescriptorSet(*descSetAllocedEntry);
                    }
                }

                poolInfo->allocedSetsToBoxed.erase(pDescriptorSets[i]);

                mDescriptorSetInfo.erase(pDescriptorSets[i]);
            }
        }

        return res;
    }

    void on_vkUpdateDescriptorSets(android::base::BumpPool* pool, VkDevice boxed_device,
                                   uint32_t descriptorWriteCount,
                                   const VkWriteDescriptorSet* pDescriptorWrites,
                                   uint32_t descriptorCopyCount,
                                   const VkCopyDescriptorSet* pDescriptorCopies) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        on_vkUpdateDescriptorSetsImpl(pool, vk, device, descriptorWriteCount, pDescriptorWrites,
                                      descriptorCopyCount, pDescriptorCopies);
    }

    void on_vkUpdateDescriptorSetsImpl(android::base::BumpPool* pool, VulkanDispatch* vk,
                                       VkDevice device, uint32_t descriptorWriteCount,
                                       const VkWriteDescriptorSet* pDescriptorWrites,
                                       uint32_t descriptorCopyCount,
                                       const VkCopyDescriptorSet* pDescriptorCopies) {
        for (uint32_t writeIdx = 0; writeIdx < descriptorWriteCount; writeIdx++) {
            const VkWriteDescriptorSet& descriptorWrite = pDescriptorWrites[writeIdx];
            auto ite = mDescriptorSetInfo.find(descriptorWrite.dstSet);
            if (ite == mDescriptorSetInfo.end()) {
                continue;
            }
            DescriptorSetInfo& descriptorSetInfo = ite->second;
            auto& table = descriptorSetInfo.allWrites;
            VkDescriptorType descType = descriptorWrite.descriptorType;
            uint32_t dstBinding = descriptorWrite.dstBinding;
            uint32_t dstArrayElement = descriptorWrite.dstArrayElement;
            uint32_t descriptorCount = descriptorWrite.descriptorCount;

            uint32_t arrOffset = dstArrayElement;

            if (isDescriptorTypeImageInfo(descType)) {
                for (uint32_t writeElemIdx = 0; writeElemIdx < descriptorCount;
                     ++writeElemIdx, ++arrOffset) {
                    // Descriptor writes wrap to the next binding. See
                    // https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VkWriteDescriptorSet.html
                    if (arrOffset >= table[dstBinding].size()) {
                        ++dstBinding;
                        arrOffset = 0;
                    }
                    auto& entry = table[dstBinding][arrOffset];
                    entry.imageInfo = descriptorWrite.pImageInfo[writeElemIdx];
                    entry.writeType = DescriptorSetInfo::DescriptorWriteType::ImageInfo;
                    entry.descriptorType = descType;
                    entry.alives.clear();
                    entry.boundColorBuffer.reset();
                    if (descriptorTypeContainsImage(descType)) {
                        auto* imageViewInfo =
                            android::base::find(mImageViewInfo, entry.imageInfo.imageView);
                        if (imageViewInfo) {
                            entry.alives.push_back(imageViewInfo->alive);
                            entry.boundColorBuffer = imageViewInfo->boundColorBuffer;
                        }
                    }
                    if (descriptorTypeContainsSampler(descType)) {
                        auto* samplerInfo =
                            android::base::find(mSamplerInfo, entry.imageInfo.sampler);
                        if (samplerInfo) {
                            entry.alives.push_back(samplerInfo->alive);
                        }
                    }
                }
            } else if (isDescriptorTypeBufferInfo(descType)) {
                for (uint32_t writeElemIdx = 0; writeElemIdx < descriptorCount;
                     ++writeElemIdx, ++arrOffset) {
                    if (arrOffset >= table[dstBinding].size()) {
                        ++dstBinding;
                        arrOffset = 0;
                    }
                    auto& entry = table[dstBinding][arrOffset];
                    entry.bufferInfo = descriptorWrite.pBufferInfo[writeElemIdx];
                    entry.writeType = DescriptorSetInfo::DescriptorWriteType::BufferInfo;
                    entry.descriptorType = descType;
                    entry.alives.clear();
                    auto* bufferInfo = android::base::find(mBufferInfo, entry.bufferInfo.buffer);
                    if (bufferInfo) {
                        entry.alives.push_back(bufferInfo->alive);
                    }
                }
            } else if (isDescriptorTypeBufferView(descType)) {
                for (uint32_t writeElemIdx = 0; writeElemIdx < descriptorCount;
                     ++writeElemIdx, ++arrOffset) {
                    if (arrOffset >= table[dstBinding].size()) {
                        ++dstBinding;
                        arrOffset = 0;
                    }
                    auto& entry = table[dstBinding][arrOffset];
                    entry.bufferView = descriptorWrite.pTexelBufferView[writeElemIdx];
                    entry.writeType = DescriptorSetInfo::DescriptorWriteType::BufferView;
                    entry.descriptorType = descType;
                    // TODO: check alive
                    ERR("%s: Snapshot for texel buffer view is incomplete.\n", __func__);
                }
            } else if (isDescriptorTypeInlineUniformBlock(descType)) {
                const VkWriteDescriptorSetInlineUniformBlock* descInlineUniformBlock =
                    static_cast<const VkWriteDescriptorSetInlineUniformBlock*>(
                        descriptorWrite.pNext);
                while (descInlineUniformBlock &&
                       descInlineUniformBlock->sType !=
                           VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK) {
                    descInlineUniformBlock =
                        static_cast<const VkWriteDescriptorSetInlineUniformBlock*>(
                            descInlineUniformBlock->pNext);
                }
                if (!descInlineUniformBlock) {
                    GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                        << __func__ << ": did not find inline uniform block";
                    return;
                }
                auto& entry = table[dstBinding][0];
                entry.inlineUniformBlock = *descInlineUniformBlock;
                entry.inlineUniformBlockBuffer.assign(
                    static_cast<const uint8_t*>(descInlineUniformBlock->pData),
                    static_cast<const uint8_t*>(descInlineUniformBlock->pData) +
                        descInlineUniformBlock->dataSize);
                entry.writeType = DescriptorSetInfo::DescriptorWriteType::InlineUniformBlock;
                entry.descriptorType = descType;
                entry.dstArrayElement = dstArrayElement;
            } else if (isDescriptorTypeAccelerationStructure(descType)) {
                // TODO
                // Look for pNext inline uniform block or acceleration structure.
                // Append new DescriptorWrite entry that holds the buffer
                ERR("%s: Ignoring Snapshot for emulated write for descriptor type 0x%x\n", __func__,
                    descType);
            }
        }
        // TODO: bookkeep pDescriptorCopies
        // Our primary use case vkQueueCommitDescriptorSetUpdatesGOOGLE does not use
        // pDescriptorCopies. Thus skip its implementation for now.
        if (descriptorCopyCount && snapshotsEnabled()) {
            ERR("%s: Snapshot does not support descriptor copy yet\n");
        }
        bool needEmulateWriteDescriptor = false;
        // c++ seems to allow for 0-size array allocation
        std::unique_ptr<bool[]> descriptorWritesNeedDeepCopy(new bool[descriptorWriteCount]);
        for (uint32_t i = 0; i < descriptorWriteCount; i++) {
            const VkWriteDescriptorSet& descriptorWrite = pDescriptorWrites[i];
            auto descriptorSetInfo =
                android::base::find(mDescriptorSetInfo, descriptorWrite.dstSet);
            descriptorWritesNeedDeepCopy[i] = false;
            if (!vk_util::vk_descriptor_type_has_image_view(descriptorWrite.descriptorType)) {
                continue;
            }
            for (uint32_t j = 0; j < descriptorWrite.descriptorCount; j++) {
                const VkDescriptorImageInfo& imageInfo = descriptorWrite.pImageInfo[j];
                const auto* imgViewInfo = android::base::find(mImageViewInfo, imageInfo.imageView);
                if (!imgViewInfo) {
                    continue;
                }
                if (descriptorWrite.descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                    continue;
                }
                const auto* samplerInfo = android::base::find(mSamplerInfo, imageInfo.sampler);
                if (samplerInfo && imgViewInfo->needEmulatedAlpha &&
                    samplerInfo->needEmulatedAlpha) {
                    needEmulateWriteDescriptor = true;
                    descriptorWritesNeedDeepCopy[i] = true;
                    break;
                }
            }
        }
        if (!needEmulateWriteDescriptor) {
            vk->vkUpdateDescriptorSets(device, descriptorWriteCount, pDescriptorWrites,
                                       descriptorCopyCount, pDescriptorCopies);
            return;
        }
        std::list<std::unique_ptr<VkDescriptorImageInfo[]>> imageInfoPool;
        std::unique_ptr<VkWriteDescriptorSet[]> descriptorWrites(
            new VkWriteDescriptorSet[descriptorWriteCount]);
        for (uint32_t i = 0; i < descriptorWriteCount; i++) {
            const VkWriteDescriptorSet& srcDescriptorWrite = pDescriptorWrites[i];
            VkWriteDescriptorSet& dstDescriptorWrite = descriptorWrites[i];
            // Shallow copy first
            dstDescriptorWrite = srcDescriptorWrite;
            if (!descriptorWritesNeedDeepCopy[i]) {
                continue;
            }
            // Deep copy
            assert(dstDescriptorWrite.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            imageInfoPool.emplace_back(
                new VkDescriptorImageInfo[dstDescriptorWrite.descriptorCount]);
            VkDescriptorImageInfo* imageInfos = imageInfoPool.back().get();
            memcpy(imageInfos, srcDescriptorWrite.pImageInfo,
                   dstDescriptorWrite.descriptorCount * sizeof(VkDescriptorImageInfo));
            dstDescriptorWrite.pImageInfo = imageInfos;
            for (uint32_t j = 0; j < dstDescriptorWrite.descriptorCount; j++) {
                VkDescriptorImageInfo& imageInfo = imageInfos[j];
                const auto* imgViewInfo = android::base::find(mImageViewInfo, imageInfo.imageView);
                auto* samplerInfo = android::base::find(mSamplerInfo, imageInfo.sampler);
                if (!imgViewInfo || !samplerInfo) continue;
                if (imgViewInfo->needEmulatedAlpha && samplerInfo->needEmulatedAlpha) {
                    if (samplerInfo->emulatedborderSampler == VK_NULL_HANDLE) {
                        // create the emulated sampler
                        VkSamplerCreateInfo createInfo;
                        deepcopy_VkSamplerCreateInfo(pool, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                                     &samplerInfo->createInfo, &createInfo);
                        switch (createInfo.borderColor) {
                            case VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:
                                createInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
                                break;
                            case VK_BORDER_COLOR_INT_TRANSPARENT_BLACK:
                                createInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
                                break;
                            case VK_BORDER_COLOR_FLOAT_CUSTOM_EXT:
                            case VK_BORDER_COLOR_INT_CUSTOM_EXT: {
                                VkSamplerCustomBorderColorCreateInfoEXT*
                                    customBorderColorCreateInfo =
                                        vk_find_struct<VkSamplerCustomBorderColorCreateInfoEXT>(
                                            &createInfo);
                                if (customBorderColorCreateInfo) {
                                    switch (createInfo.borderColor) {
                                        case VK_BORDER_COLOR_FLOAT_CUSTOM_EXT:
                                            customBorderColorCreateInfo->customBorderColor
                                                .float32[3] = 1.0f;
                                            break;
                                        case VK_BORDER_COLOR_INT_CUSTOM_EXT:
                                            customBorderColorCreateInfo->customBorderColor
                                                .int32[3] = 128;
                                            break;
                                        default:
                                            break;
                                    }
                                }
                                break;
                            }
                            default:
                                break;
                        }
                        vk->vkCreateSampler(device, &createInfo, nullptr,
                                            &samplerInfo->emulatedborderSampler);
                    }
                    imageInfo.sampler = samplerInfo->emulatedborderSampler;
                }
            }
        }
        vk->vkUpdateDescriptorSets(device, descriptorWriteCount, descriptorWrites.get(),
                                   descriptorCopyCount, pDescriptorCopies);
    }

    VkResult on_vkCreateShaderModule(android::base::BumpPool* pool, VkDevice boxed_device,
                                     const VkShaderModuleCreateInfo* pCreateInfo,
                                     const VkAllocationCallbacks* pAllocator,
                                     VkShaderModule* pShaderModule) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        VkResult result =
            deviceDispatch->vkCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
        if (result != VK_SUCCESS) {
            return result;
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto& shaderModuleInfo = mShaderModuleInfo[*pShaderModule];
        shaderModuleInfo.device = device;

        *pShaderModule = new_boxed_non_dispatchable_VkShaderModule(*pShaderModule);

        return result;
    }

    void destroyShaderModuleLocked(VkDevice device, VulkanDispatch* deviceDispatch,
                                   VkShaderModule shaderModule,
                                   const VkAllocationCallbacks* pAllocator) {
        deviceDispatch->vkDestroyShaderModule(device, shaderModule, pAllocator);

        mShaderModuleInfo.erase(shaderModule);
    }

    void on_vkDestroyShaderModule(android::base::BumpPool* pool, VkDevice boxed_device,
                                  VkShaderModule shaderModule,
                                  const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        destroyShaderModuleLocked(device, deviceDispatch, shaderModule, pAllocator);
    }

    VkResult on_vkCreatePipelineCache(android::base::BumpPool* pool, VkDevice boxed_device,
                                      const VkPipelineCacheCreateInfo* pCreateInfo,
                                      const VkAllocationCallbacks* pAllocator,
                                      VkPipelineCache* pPipelineCache) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        VkResult result =
            deviceDispatch->vkCreatePipelineCache(device, pCreateInfo, pAllocator, pPipelineCache);
        if (result != VK_SUCCESS) {
            return result;
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto& pipelineCacheInfo = mPipelineCacheInfo[*pPipelineCache];
        pipelineCacheInfo.device = device;

        *pPipelineCache = new_boxed_non_dispatchable_VkPipelineCache(*pPipelineCache);

        return result;
    }

    void destroyPipelineCacheLocked(VkDevice device, VulkanDispatch* deviceDispatch,
                                    VkPipelineCache pipelineCache,
                                    const VkAllocationCallbacks* pAllocator) {
        deviceDispatch->vkDestroyPipelineCache(device, pipelineCache, pAllocator);

        mPipelineCacheInfo.erase(pipelineCache);
    }

    void on_vkDestroyPipelineCache(android::base::BumpPool* pool, VkDevice boxed_device,
                                   VkPipelineCache pipelineCache,
                                   const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        destroyPipelineCacheLocked(device, deviceDispatch, pipelineCache, pAllocator);
    }

    VkResult on_vkCreateGraphicsPipelines(android::base::BumpPool* pool, VkDevice boxed_device,
                                          VkPipelineCache pipelineCache, uint32_t createInfoCount,
                                          const VkGraphicsPipelineCreateInfo* pCreateInfos,
                                          const VkAllocationCallbacks* pAllocator,
                                          VkPipeline* pPipelines) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        VkResult result = deviceDispatch->vkCreateGraphicsPipelines(
            device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
        if (result != VK_SUCCESS && result != VK_PIPELINE_COMPILE_REQUIRED) {
            return result;
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);

        for (uint32_t i = 0; i < createInfoCount; i++) {
            if (!pPipelines[i]) {
                continue;
            }
            auto& pipelineInfo = mPipelineInfo[pPipelines[i]];
            pipelineInfo.device = device;

            pPipelines[i] = new_boxed_non_dispatchable_VkPipeline(pPipelines[i]);
        }

        return result;
    }

    void destroyPipelineLocked(VkDevice device, VulkanDispatch* deviceDispatch, VkPipeline pipeline,
                               const VkAllocationCallbacks* pAllocator) {
        deviceDispatch->vkDestroyPipeline(device, pipeline, pAllocator);

        mPipelineInfo.erase(pipeline);
    }

    void on_vkDestroyPipeline(android::base::BumpPool* pool, VkDevice boxed_device,
                              VkPipeline pipeline, const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        destroyPipelineLocked(device, deviceDispatch, pipeline, pAllocator);
    }

    void on_vkCmdCopyImage(android::base::BumpPool* pool, VkCommandBuffer boxed_commandBuffer,
                           VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage,
                           VkImageLayout dstImageLayout, uint32_t regionCount,
                           const VkImageCopy* pRegions) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* srcImg = android::base::find(mImageInfo, srcImage);
        auto* dstImg = android::base::find(mImageInfo, dstImage);
        if (!srcImg || !dstImg) return;

        VkDevice device = srcImg->cmpInfo.device();
        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return;

        bool needEmulatedSrc = deviceInfo->needEmulatedDecompression(srcImg->cmpInfo);
        bool needEmulatedDst = deviceInfo->needEmulatedDecompression(dstImg->cmpInfo);
        if (!needEmulatedSrc && !needEmulatedDst) {
            vk->vkCmdCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout,
                               regionCount, pRegions);
            return;
        }
        VkImage srcImageMip = srcImage;
        VkImage dstImageMip = dstImage;
        for (uint32_t r = 0; r < regionCount; r++) {
            if (needEmulatedSrc) {
                srcImageMip = srcImg->cmpInfo.compressedMipmap(pRegions[r].srcSubresource.mipLevel);
            }
            if (needEmulatedDst) {
                dstImageMip = dstImg->cmpInfo.compressedMipmap(pRegions[r].dstSubresource.mipLevel);
            }
            VkImageCopy region = CompressedImageInfo::getCompressedMipmapsImageCopy(
                pRegions[r], srcImg->cmpInfo, dstImg->cmpInfo, needEmulatedSrc, needEmulatedDst);
            vk->vkCmdCopyImage(commandBuffer, srcImageMip, srcImageLayout, dstImageMip,
                               dstImageLayout, 1, &region);
        }
    }

    void on_vkCmdCopyImageToBuffer(android::base::BumpPool* pool,
                                   VkCommandBuffer boxed_commandBuffer, VkImage srcImage,
                                   VkImageLayout srcImageLayout, VkBuffer dstBuffer,
                                   uint32_t regionCount, const VkBufferImageCopy* pRegions) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* imageInfo = android::base::find(mImageInfo, srcImage);
        auto* bufferInfo = android::base::find(mBufferInfo, dstBuffer);
        if (!imageInfo || !bufferInfo) return;
        auto* deviceInfo = android::base::find(mDeviceInfo, bufferInfo->device);
        if (!deviceInfo) return;
        CompressedImageInfo& cmpInfo = imageInfo->cmpInfo;
        if (!deviceInfo->needEmulatedDecompression(cmpInfo)) {
            vk->vkCmdCopyImageToBuffer(commandBuffer, srcImage, srcImageLayout, dstBuffer,
                                       regionCount, pRegions);
            return;
        }
        for (uint32_t r = 0; r < regionCount; r++) {
            uint32_t mipLevel = pRegions[r].imageSubresource.mipLevel;
            VkBufferImageCopy region = cmpInfo.getBufferImageCopy(pRegions[r]);
            vk->vkCmdCopyImageToBuffer(commandBuffer, cmpInfo.compressedMipmap(mipLevel),
                                       srcImageLayout, dstBuffer, 1, &region);
        }
    }

    void on_vkCmdCopyImage2(android::base::BumpPool* pool,
                           VkCommandBuffer boxed_commandBuffer,
                           const VkCopyImageInfo2* pCopyImageInfo) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* srcImg = android::base::find(mImageInfo, pCopyImageInfo->srcImage);
        auto* dstImg = android::base::find(mImageInfo, pCopyImageInfo->dstImage);
        if (!srcImg || !dstImg) return;

        VkDevice device = srcImg->cmpInfo.device();
        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return;

        bool needEmulatedSrc = deviceInfo->needEmulatedDecompression(srcImg->cmpInfo);
        bool needEmulatedDst = deviceInfo->needEmulatedDecompression(dstImg->cmpInfo);
        if (!needEmulatedSrc && !needEmulatedDst) {
            vk->vkCmdCopyImage2(commandBuffer, pCopyImageInfo);
            return;
        }
        VkImage srcImageMip = pCopyImageInfo->srcImage;
        VkImage dstImageMip = pCopyImageInfo->dstImage;
        for (uint32_t r = 0; r < pCopyImageInfo->regionCount; r++) {
            if (needEmulatedSrc) {
                srcImageMip = srcImg->cmpInfo.compressedMipmap(pCopyImageInfo->pRegions[r].srcSubresource.mipLevel);
            }
            if (needEmulatedDst) {
                dstImageMip = dstImg->cmpInfo.compressedMipmap(pCopyImageInfo->pRegions[r].dstSubresource.mipLevel);
            }

            VkCopyImageInfo2 inf2 = *pCopyImageInfo;
            inf2.regionCount = 1;
            inf2.srcImage = srcImageMip;
            inf2.dstImage = dstImageMip;

            VkImageCopy2 region = CompressedImageInfo::getCompressedMipmapsImageCopy(
                pCopyImageInfo->pRegions[r], srcImg->cmpInfo, dstImg->cmpInfo, needEmulatedSrc, needEmulatedDst);
            inf2.pRegions = &region;

            vk->vkCmdCopyImage2(commandBuffer, &inf2);
        }
    }

    void on_vkCmdCopyImageToBuffer2(android::base::BumpPool* pool,
                                   VkCommandBuffer boxed_commandBuffer,
                                   const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* imageInfo = android::base::find(mImageInfo, pCopyImageToBufferInfo->srcImage);
        auto* bufferInfo = android::base::find(mBufferInfo, pCopyImageToBufferInfo->dstBuffer);
        if (!imageInfo || !bufferInfo) return;
        auto* deviceInfo = android::base::find(mDeviceInfo, bufferInfo->device);
        if (!deviceInfo) return;
        CompressedImageInfo& cmpInfo = imageInfo->cmpInfo;
        if (!deviceInfo->needEmulatedDecompression(cmpInfo)) {
            vk->vkCmdCopyImageToBuffer2(commandBuffer, pCopyImageToBufferInfo);
            return;
        }
        for (uint32_t r = 0; r < pCopyImageToBufferInfo->regionCount; r++) {
            uint32_t mipLevel = pCopyImageToBufferInfo->pRegions[r].imageSubresource.mipLevel;
            VkBufferImageCopy2 region = cmpInfo.getBufferImageCopy(pCopyImageToBufferInfo->pRegions[r]);
            VkCopyImageToBufferInfo2 inf = *pCopyImageToBufferInfo;
            inf.regionCount = 1;
            inf.pRegions = &region;
            inf.srcImage = cmpInfo.compressedMipmap(mipLevel);

            vk->vkCmdCopyImageToBuffer2(commandBuffer, &inf);
        }
    }

    void on_vkCmdCopyImage2KHR(android::base::BumpPool* pool,
                           VkCommandBuffer boxed_commandBuffer,
                           const VkCopyImageInfo2KHR* pCopyImageInfo) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* srcImg = android::base::find(mImageInfo, pCopyImageInfo->srcImage);
        auto* dstImg = android::base::find(mImageInfo, pCopyImageInfo->dstImage);
        if (!srcImg || !dstImg) return;

        VkDevice device = srcImg->cmpInfo.device();
        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return;

        bool needEmulatedSrc = deviceInfo->needEmulatedDecompression(srcImg->cmpInfo);
        bool needEmulatedDst = deviceInfo->needEmulatedDecompression(dstImg->cmpInfo);
        if (!needEmulatedSrc && !needEmulatedDst) {
            vk->vkCmdCopyImage2KHR(commandBuffer, pCopyImageInfo);
            return;
        }
        VkImage srcImageMip = pCopyImageInfo->srcImage;
        VkImage dstImageMip = pCopyImageInfo->dstImage;
        for (uint32_t r = 0; r < pCopyImageInfo->regionCount; r++) {
            if (needEmulatedSrc) {
                srcImageMip = srcImg->cmpInfo.compressedMipmap(pCopyImageInfo->pRegions[r].srcSubresource.mipLevel);
            }
            if (needEmulatedDst) {
                dstImageMip = dstImg->cmpInfo.compressedMipmap(pCopyImageInfo->pRegions[r].dstSubresource.mipLevel);
            }

            VkCopyImageInfo2KHR inf2 = *pCopyImageInfo;
            inf2.regionCount = 1;
            inf2.srcImage = srcImageMip;
            inf2.dstImage = dstImageMip;

            VkImageCopy2KHR region = CompressedImageInfo::getCompressedMipmapsImageCopy(
                pCopyImageInfo->pRegions[r], srcImg->cmpInfo, dstImg->cmpInfo, needEmulatedSrc, needEmulatedDst);
            inf2.pRegions = &region;

            vk->vkCmdCopyImage2KHR(commandBuffer, &inf2);
        }
    }

    void on_vkCmdCopyImageToBuffer2KHR(android::base::BumpPool* pool,
                                   VkCommandBuffer boxed_commandBuffer,
                                   const VkCopyImageToBufferInfo2KHR* pCopyImageToBufferInfo) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* imageInfo = android::base::find(mImageInfo, pCopyImageToBufferInfo->srcImage);
        auto* bufferInfo = android::base::find(mBufferInfo, pCopyImageToBufferInfo->dstBuffer);
        if (!imageInfo || !bufferInfo) return;
        auto* deviceInfo = android::base::find(mDeviceInfo, bufferInfo->device);
        if (!deviceInfo) return;
        CompressedImageInfo& cmpInfo = imageInfo->cmpInfo;
        if (!deviceInfo->needEmulatedDecompression(cmpInfo)) {
            vk->vkCmdCopyImageToBuffer2KHR(commandBuffer, pCopyImageToBufferInfo);
            return;
        }
        for (uint32_t r = 0; r < pCopyImageToBufferInfo->regionCount; r++) {
            uint32_t mipLevel = pCopyImageToBufferInfo->pRegions[r].imageSubresource.mipLevel;
            VkBufferImageCopy2KHR region = cmpInfo.getBufferImageCopy(pCopyImageToBufferInfo->pRegions[r]);
            VkCopyImageToBufferInfo2KHR inf = *pCopyImageToBufferInfo;
            inf.regionCount = 1;
            inf.pRegions = &region;
            inf.srcImage = cmpInfo.compressedMipmap(mipLevel);

            vk->vkCmdCopyImageToBuffer2KHR(commandBuffer, &inf);
        }
    }

    void on_vkGetImageMemoryRequirements(android::base::BumpPool* pool, VkDevice boxed_device,
                                         VkImage image, VkMemoryRequirements* pMemoryRequirements) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);
        vk->vkGetImageMemoryRequirements(device, image, pMemoryRequirements);
        std::lock_guard<std::recursive_mutex> lock(mLock);
        updateImageMemorySizeLocked(device, image, pMemoryRequirements);

        auto* physicalDevice = android::base::find(mDeviceToPhysicalDevice, device);
        if (!physicalDevice) {
            ERR("Failed to find physical device for device:%p", device);
            return;
        }

        auto* physicalDeviceInfo = android::base::find(mPhysdevInfo, *physicalDevice);
        if (!physicalDeviceInfo) {
            ERR("Failed to find physical device info for physical device:%p", *physicalDevice);
            return;
        }

        auto& physicalDeviceMemHelper = physicalDeviceInfo->memoryPropertiesHelper;
        physicalDeviceMemHelper->transformToGuestMemoryRequirements(pMemoryRequirements);
    }

    void on_vkGetImageMemoryRequirements2(android::base::BumpPool* pool, VkDevice boxed_device,
                                          const VkImageMemoryRequirementsInfo2* pInfo,
                                          VkMemoryRequirements2* pMemoryRequirements) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* physicalDevice = android::base::find(mDeviceToPhysicalDevice, device);
        if (!physicalDevice) {
            ERR("Failed to find physical device for device:%p", device);
            return;
        }

        auto* physicalDeviceInfo = android::base::find(mPhysdevInfo, *physicalDevice);
        if (!physicalDeviceInfo) {
            ERR("Failed to find physical device info for physical device:%p", *physicalDevice);
            return;
        }

        if ((physicalDeviceInfo->props.apiVersion >= VK_MAKE_VERSION(1, 1, 0)) &&
            vk->vkGetImageMemoryRequirements2) {
            vk->vkGetImageMemoryRequirements2(device, pInfo, pMemoryRequirements);
        } else if (hasDeviceExtension(device, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME)) {
            vk->vkGetImageMemoryRequirements2KHR(device, pInfo, pMemoryRequirements);
        } else {
            if (pInfo->pNext) {
                ERR("Warning: trying to use extension struct in VkMemoryRequirements2 without "
                    "having enabled the extension!");
            }

            vk->vkGetImageMemoryRequirements(device, pInfo->image,
                                             &pMemoryRequirements->memoryRequirements);
        }

        updateImageMemorySizeLocked(device, pInfo->image, &pMemoryRequirements->memoryRequirements);

        auto& physicalDeviceMemHelper = physicalDeviceInfo->memoryPropertiesHelper;
        physicalDeviceMemHelper->transformToGuestMemoryRequirements(
            &pMemoryRequirements->memoryRequirements);
    }

    void on_vkGetBufferMemoryRequirements(android::base::BumpPool* pool, VkDevice boxed_device,
                                          VkBuffer buffer,
                                          VkMemoryRequirements* pMemoryRequirements) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);
        vk->vkGetBufferMemoryRequirements(device, buffer, pMemoryRequirements);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* physicalDevice = android::base::find(mDeviceToPhysicalDevice, device);
        if (!physicalDevice) {
            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                << "No physical device available for " << device;
        }

        auto* physicalDeviceInfo = android::base::find(mPhysdevInfo, *physicalDevice);
        if (!physicalDeviceInfo) {
            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                << "No physical device info available for " << *physicalDevice;
        }

        auto& physicalDeviceMemHelper = physicalDeviceInfo->memoryPropertiesHelper;
        physicalDeviceMemHelper->transformToGuestMemoryRequirements(pMemoryRequirements);
    }

    void on_vkGetBufferMemoryRequirements2(android::base::BumpPool* pool, VkDevice boxed_device,
                                           const VkBufferMemoryRequirementsInfo2* pInfo,
                                           VkMemoryRequirements2* pMemoryRequirements) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* physicalDevice = android::base::find(mDeviceToPhysicalDevice, device);
        if (!physicalDevice) {
            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                << "No physical device available for " << device;
        }

        auto* physicalDeviceInfo = android::base::find(mPhysdevInfo, *physicalDevice);
        if (!physicalDeviceInfo) {
            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                << "No physical device info available for " << *physicalDevice;
        }

        if ((physicalDeviceInfo->props.apiVersion >= VK_MAKE_VERSION(1, 1, 0)) &&
            vk->vkGetBufferMemoryRequirements2) {
            vk->vkGetBufferMemoryRequirements2(device, pInfo, pMemoryRequirements);
        } else if (hasDeviceExtension(device, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME)) {
            vk->vkGetBufferMemoryRequirements2KHR(device, pInfo, pMemoryRequirements);
        } else {
            if (pInfo->pNext) {
                ERR("Warning: trying to use extension struct in VkMemoryRequirements2 without "
                    "having enabled the extension!");
            }

            vk->vkGetBufferMemoryRequirements(device, pInfo->buffer,
                                              &pMemoryRequirements->memoryRequirements);
        }

        auto& physicalDeviceMemHelper = physicalDeviceInfo->memoryPropertiesHelper;
        physicalDeviceMemHelper->transformToGuestMemoryRequirements(
            &pMemoryRequirements->memoryRequirements);
    }

    void on_vkCmdCopyBufferToImage(android::base::BumpPool* pool,
                                   VkCommandBuffer boxed_commandBuffer, VkBuffer srcBuffer,
                                   VkImage dstImage, VkImageLayout dstImageLayout,
                                   uint32_t regionCount, const VkBufferImageCopy* pRegions,
                                   const VkDecoderContext& context) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* imageInfo = android::base::find(mImageInfo, dstImage);
        if (!imageInfo) return;
        auto* bufferInfo = android::base::find(mBufferInfo, srcBuffer);
        if (!bufferInfo) {
            return;
        }
        VkDevice device = bufferInfo->device;
        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) {
            return;
        }
        if (!deviceInfo->needEmulatedDecompression(imageInfo->cmpInfo)) {
            vk->vkCmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout,
                                       regionCount, pRegions);
            return;
        }
        auto* cmdBufferInfo = android::base::find(mCommandBufferInfo, commandBuffer);
        if (!cmdBufferInfo) {
            return;
        }
        CompressedImageInfo& cmpInfo = imageInfo->cmpInfo;

        for (uint32_t r = 0; r < regionCount; r++) {
            uint32_t mipLevel = pRegions[r].imageSubresource.mipLevel;
            VkBufferImageCopy region = cmpInfo.getBufferImageCopy(pRegions[r]);
            vk->vkCmdCopyBufferToImage(commandBuffer, srcBuffer, cmpInfo.compressedMipmap(mipLevel),
                                       dstImageLayout, 1, &region);
        }

        if (cmpInfo.canDecompressOnCpu()) {
            // Get a pointer to the compressed image memory
            const MemoryInfo* memoryInfo = android::base::find(mMemoryInfo, bufferInfo->memory);
            if (!memoryInfo) {
                WARN("ASTC CPU decompression: couldn't find mapped memory info");
                return;
            }
            if (!memoryInfo->ptr) {
                WARN("ASTC CPU decompression: VkBuffer memory isn't host-visible");
                return;
            }
            uint8_t* astcData = (uint8_t*)(memoryInfo->ptr) + bufferInfo->memoryOffset;
            cmpInfo.decompressOnCpu(commandBuffer, astcData, bufferInfo->size, dstImage,
                                    dstImageLayout, regionCount, pRegions, context);
        }
    }

    void on_vkCmdCopyBufferToImage2(android::base::BumpPool* pool,
                                    VkCommandBuffer boxed_commandBuffer,
                                    const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo,
                                    const VkDecoderContext& context) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* imageInfo = android::base::find(mImageInfo, pCopyBufferToImageInfo->dstImage);
        if (!imageInfo) return;
        auto* bufferInfo = android::base::find(mBufferInfo, pCopyBufferToImageInfo->srcBuffer);
        if (!bufferInfo) {
            return;
        }
        VkDevice device = bufferInfo->device;
        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) {
            return;
        }
        if (!deviceInfo->needEmulatedDecompression(imageInfo->cmpInfo)) {
            vk->vkCmdCopyBufferToImage2(commandBuffer, pCopyBufferToImageInfo);
            return;
        }
        auto* cmdBufferInfo = android::base::find(mCommandBufferInfo, commandBuffer);
        if (!cmdBufferInfo) {
            return;
        }
        CompressedImageInfo& cmpInfo = imageInfo->cmpInfo;

        for (uint32_t r = 0; r < pCopyBufferToImageInfo->regionCount; r++) {
            VkCopyBufferToImageInfo2 inf;
            uint32_t mipLevel = pCopyBufferToImageInfo->pRegions[r].imageSubresource.mipLevel;
            inf.dstImage = cmpInfo.compressedMipmap(mipLevel);
            VkBufferImageCopy2 region = cmpInfo.getBufferImageCopy(pCopyBufferToImageInfo->pRegions[r]);
            inf.regionCount = 1;
            inf.pRegions = &region;

            vk->vkCmdCopyBufferToImage2(commandBuffer, &inf);
        }

        if (cmpInfo.canDecompressOnCpu()) {
            // Get a pointer to the compressed image memory
            const MemoryInfo* memoryInfo = android::base::find(mMemoryInfo, bufferInfo->memory);
            if (!memoryInfo) {
                WARN("ASTC CPU decompression: couldn't find mapped memory info");
                return;
            }
            if (!memoryInfo->ptr) {
                WARN("ASTC CPU decompression: VkBuffer memory isn't host-visible");
                return;
            }
            uint8_t* astcData = (uint8_t*)(memoryInfo->ptr) + bufferInfo->memoryOffset;

            cmpInfo.decompressOnCpu(commandBuffer, astcData, bufferInfo->size, pCopyBufferToImageInfo, context);
        }
    }

    void on_vkCmdCopyBufferToImage2KHR(android::base::BumpPool* pool,
                                    VkCommandBuffer boxed_commandBuffer,
                                    const VkCopyBufferToImageInfo2KHR* pCopyBufferToImageInfo,
                                    const VkDecoderContext& context) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* imageInfo = android::base::find(mImageInfo, pCopyBufferToImageInfo->dstImage);
        if (!imageInfo) return;
        auto* bufferInfo = android::base::find(mBufferInfo, pCopyBufferToImageInfo->srcBuffer);
        if (!bufferInfo) {
            return;
        }
        VkDevice device = bufferInfo->device;
        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) {
            return;
        }
        if (!deviceInfo->needEmulatedDecompression(imageInfo->cmpInfo)) {
            vk->vkCmdCopyBufferToImage2KHR(commandBuffer, pCopyBufferToImageInfo);
            return;
        }
        auto* cmdBufferInfo = android::base::find(mCommandBufferInfo, commandBuffer);
        if (!cmdBufferInfo) {
            return;
        }
        CompressedImageInfo& cmpInfo = imageInfo->cmpInfo;

        for (uint32_t r = 0; r < pCopyBufferToImageInfo->regionCount; r++) {
            VkCopyBufferToImageInfo2KHR inf;
            uint32_t mipLevel = pCopyBufferToImageInfo->pRegions[r].imageSubresource.mipLevel;
            inf.dstImage = cmpInfo.compressedMipmap(mipLevel);
            VkBufferImageCopy2KHR region = cmpInfo.getBufferImageCopy(pCopyBufferToImageInfo->pRegions[r]);
            inf.regionCount = 1;
            inf.pRegions = &region;

            vk->vkCmdCopyBufferToImage2KHR(commandBuffer, &inf);
        }

        if (cmpInfo.canDecompressOnCpu()) {
            // Get a pointer to the compressed image memory
            const MemoryInfo* memoryInfo = android::base::find(mMemoryInfo, bufferInfo->memory);
            if (!memoryInfo) {
                WARN("ASTC CPU decompression: couldn't find mapped memory info");
                return;
            }
            if (!memoryInfo->ptr) {
                WARN("ASTC CPU decompression: VkBuffer memory isn't host-visible");
                return;
            }
            uint8_t* astcData = (uint8_t*)(memoryInfo->ptr) + bufferInfo->memoryOffset;

            cmpInfo.decompressOnCpu(commandBuffer, astcData, bufferInfo->size, pCopyBufferToImageInfo, context);
        }
    }

    inline void convertQueueFamilyForeignToExternal(uint32_t* queueFamilyIndexPtr) {
        if (*queueFamilyIndexPtr == VK_QUEUE_FAMILY_FOREIGN_EXT) {
            *queueFamilyIndexPtr = VK_QUEUE_FAMILY_EXTERNAL;
        }
    }

    inline void convertQueueFamilyForeignToExternal_VkBufferMemoryBarrier(
        VkBufferMemoryBarrier* barrier) {
        convertQueueFamilyForeignToExternal(&barrier->srcQueueFamilyIndex);
        convertQueueFamilyForeignToExternal(&barrier->dstQueueFamilyIndex);
    }

    inline void convertQueueFamilyForeignToExternal_VkImageMemoryBarrier(
        VkImageMemoryBarrier* barrier) {
        convertQueueFamilyForeignToExternal(&barrier->srcQueueFamilyIndex);
        convertQueueFamilyForeignToExternal(&barrier->dstQueueFamilyIndex);
    }

    inline VkImage getIMBImage(const VkImageMemoryBarrier& imb) { return imb.image; }
    inline VkImage getIMBImage(const VkImageMemoryBarrier2& imb) { return imb.image; }

    inline VkImageLayout getIMBNewLayout(const VkImageMemoryBarrier& imb) { return imb.newLayout; }
    inline VkImageLayout getIMBNewLayout(const VkImageMemoryBarrier2& imb) { return imb.newLayout; }

    inline uint32_t getIMBSrcQueueFamilyIndex(const VkImageMemoryBarrier& imb) {
        return imb.srcQueueFamilyIndex;
    }
    inline uint32_t getIMBSrcQueueFamilyIndex(const VkImageMemoryBarrier2& imb) {
        return imb.srcQueueFamilyIndex;
    }
    inline uint32_t getIMBDstQueueFamilyIndex(const VkImageMemoryBarrier& imb) {
        return imb.dstQueueFamilyIndex;
    }
    inline uint32_t getIMBDstQueueFamilyIndex(const VkImageMemoryBarrier2& imb) {
        return imb.dstQueueFamilyIndex;
    }

    template <typename VkImageMemoryBarrierType>
    void processImageMemoryBarrier(VkCommandBuffer commandBuffer, uint32_t imageMemoryBarrierCount,
                                   const VkImageMemoryBarrierType* pImageMemoryBarriers) {
        std::lock_guard<std::recursive_mutex> lock(mLock);
        CommandBufferInfo* cmdBufferInfo = android::base::find(mCommandBufferInfo, commandBuffer);
        if (!cmdBufferInfo) return;

        // TODO: update image layout in ImageInfo
        for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
            auto* imageInfo = android::base::find(mImageInfo, getIMBImage(pImageMemoryBarriers[i]));
            if (!imageInfo) {
                continue;
            }
            cmdBufferInfo->imageLayouts[getIMBImage(pImageMemoryBarriers[i])] =
                getIMBNewLayout(pImageMemoryBarriers[i]);
            if (!imageInfo->boundColorBuffer.has_value()) {
                continue;
            }
            HandleType cb = imageInfo->boundColorBuffer.value();
            if (getIMBSrcQueueFamilyIndex(pImageMemoryBarriers[i]) == VK_QUEUE_FAMILY_EXTERNAL) {
                cmdBufferInfo->acquiredColorBuffers.insert(cb);
            }
            if (getIMBDstQueueFamilyIndex(pImageMemoryBarriers[i]) == VK_QUEUE_FAMILY_EXTERNAL) {
                cmdBufferInfo->releasedColorBuffers.insert(cb);
            }
            cmdBufferInfo->cbLayouts[cb] = getIMBNewLayout(pImageMemoryBarriers[i]);
            // Insert unconditionally to this list, regardless of whether or not
            // there is a queue family ownership transfer
            cmdBufferInfo->imageBarrierColorBuffers.insert(cb);
        }
    }

    void on_vkCmdPipelineBarrier(android::base::BumpPool* pool, VkCommandBuffer boxed_commandBuffer,
                                 VkPipelineStageFlags srcStageMask,
                                 VkPipelineStageFlags dstStageMask,
                                 VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount,
                                 const VkMemoryBarrier* pMemoryBarriers,
                                 uint32_t bufferMemoryBarrierCount,
                                 const VkBufferMemoryBarrier* pBufferMemoryBarriers,
                                 uint32_t imageMemoryBarrierCount,
                                 const VkImageMemoryBarrier* pImageMemoryBarriers) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        for (uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
            convertQueueFamilyForeignToExternal_VkBufferMemoryBarrier(
                ((VkBufferMemoryBarrier*)pBufferMemoryBarriers) + i);
        }

        for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
            convertQueueFamilyForeignToExternal_VkImageMemoryBarrier(
                ((VkImageMemoryBarrier*)pImageMemoryBarriers) + i);
        }

        if (imageMemoryBarrierCount == 0) {
            vk->vkCmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, dependencyFlags,
                                     memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount,
                                     pBufferMemoryBarriers, imageMemoryBarrierCount,
                                     pImageMemoryBarriers);
            return;
        }
        std::lock_guard<std::recursive_mutex> lock(mLock);
        CommandBufferInfo* cmdBufferInfo = android::base::find(mCommandBufferInfo, commandBuffer);
        if (!cmdBufferInfo) return;

        DeviceInfo* deviceInfo = android::base::find(mDeviceInfo, cmdBufferInfo->device);
        if (!deviceInfo) return;

        processImageMemoryBarrier(commandBuffer, imageMemoryBarrierCount, pImageMemoryBarriers);

        if (!deviceInfo->emulateTextureEtc2 && !deviceInfo->emulateTextureAstc) {
            vk->vkCmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, dependencyFlags,
                                     memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount,
                                     pBufferMemoryBarriers, imageMemoryBarrierCount,
                                     pImageMemoryBarriers);
            return;
        }

        // This is a compressed image. Handle decompression before calling vkCmdPipelineBarrier

        std::vector<VkImageMemoryBarrier> imageBarriers;
        bool needRebind = false;

        for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
            const VkImageMemoryBarrier& srcBarrier = pImageMemoryBarriers[i];
            auto* imageInfo = android::base::find(mImageInfo, srcBarrier.image);

            // If the image doesn't need GPU decompression, nothing to do.
            if (!imageInfo || !deviceInfo->needGpuDecompression(imageInfo->cmpInfo)) {
                imageBarriers.push_back(srcBarrier);
                continue;
            }

            // Otherwise, decompress the image, if we're going to read from it.
            needRebind |= imageInfo->cmpInfo.decompressIfNeeded(
                vk, commandBuffer, srcStageMask, dstStageMask, srcBarrier, imageBarriers);
        }

        if (needRebind && cmdBufferInfo->computePipeline) {
            // Recover pipeline bindings
            // TODO(gregschlom): instead of doing this here again and again after each image we
            // decompress, could we do it once before calling vkCmdDispatch?
            vk->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  cmdBufferInfo->computePipeline);
            if (!cmdBufferInfo->currentDescriptorSets.empty()) {
                vk->vkCmdBindDescriptorSets(
                    commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cmdBufferInfo->descriptorLayout,
                    cmdBufferInfo->firstSet, cmdBufferInfo->currentDescriptorSets.size(),
                    cmdBufferInfo->currentDescriptorSets.data(),
                    cmdBufferInfo->dynamicOffsets.size(), cmdBufferInfo->dynamicOffsets.data());
            }
        }

        // Apply the remaining barriers
        if (memoryBarrierCount || bufferMemoryBarrierCount || !imageBarriers.empty()) {
            vk->vkCmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, dependencyFlags,
                                     memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount,
                                     pBufferMemoryBarriers, imageBarriers.size(),
                                     imageBarriers.data());
        }
    }

    void on_vkCmdPipelineBarrier2(android::base::BumpPool* pool,
                                  VkCommandBuffer boxed_commandBuffer,
                                  const VkDependencyInfo* pDependencyInfo) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        for (uint32_t i = 0; i < pDependencyInfo->bufferMemoryBarrierCount; ++i) {
            convertQueueFamilyForeignToExternal_VkBufferMemoryBarrier(
                ((VkBufferMemoryBarrier*)pDependencyInfo->pBufferMemoryBarriers) + i);
        }

        for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; ++i) {
            convertQueueFamilyForeignToExternal_VkImageMemoryBarrier(
                ((VkImageMemoryBarrier*)pDependencyInfo->pImageMemoryBarriers) + i);
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);
        CommandBufferInfo* cmdBufferInfo = android::base::find(mCommandBufferInfo, commandBuffer);
        if (!cmdBufferInfo) return;

        DeviceInfo* deviceInfo = android::base::find(mDeviceInfo, cmdBufferInfo->device);
        if (!deviceInfo) return;

        processImageMemoryBarrier(commandBuffer, pDependencyInfo->imageMemoryBarrierCount,
                                  pDependencyInfo->pImageMemoryBarriers);

        // TODO: If this is a decompressed image, handle decompression before calling
        // VkCmdvkCmdPipelineBarrier2 i.e. match on_vkCmdPipelineBarrier implementation
        vk->vkCmdPipelineBarrier2(commandBuffer, pDependencyInfo);
    }

    bool mapHostVisibleMemoryToGuestPhysicalAddressLocked(VulkanDispatch* vk, VkDevice device,
                                                          VkDeviceMemory memory,
                                                          uint64_t physAddr) {
        if (!m_emu->features.GlDirectMem.enabled &&
            !m_emu->features.VirtioGpuNext.enabled) {
            // fprintf(stderr, "%s: Tried to use direct mapping "
            // "while GlDirectMem is not enabled!\n");
        }

        auto* info = android::base::find(mMemoryInfo, memory);
        if (!info) return false;

        info->guestPhysAddr = physAddr;

        constexpr size_t kPageBits = 12;
        constexpr size_t kPageSize = 1u << kPageBits;
        constexpr size_t kPageOffsetMask = kPageSize - 1;

        uintptr_t addr = reinterpret_cast<uintptr_t>(info->ptr);
        uintptr_t pageOffset = addr & kPageOffsetMask;

        info->pageAlignedHva = reinterpret_cast<void*>(addr - pageOffset);
        info->sizeToPage = ((info->size + pageOffset + kPageSize - 1) >> kPageBits) << kPageBits;

        if (mLogging) {
            fprintf(stderr, "%s: map: %p, %p -> [0x%llx 0x%llx]\n", __func__, info->ptr,
                    info->pageAlignedHva, (unsigned long long)info->guestPhysAddr,
                    (unsigned long long)info->guestPhysAddr + info->sizeToPage);
        }

        info->directMapped = true;
        uint64_t gpa = info->guestPhysAddr;
        void* hva = info->pageAlignedHva;
        size_t sizeToPage = info->sizeToPage;

        AutoLock occupiedGpasLock(mOccupiedGpasLock);

        auto* existingMemoryInfo = android::base::find(mOccupiedGpas, gpa);
        if (existingMemoryInfo) {
            fprintf(stderr, "%s: WARNING: already mapped gpa 0x%llx, replacing", __func__,
                    (unsigned long long)gpa);

            get_emugl_vm_operations().unmapUserBackedRam(existingMemoryInfo->gpa,
                                                         existingMemoryInfo->sizeToPage);

            mOccupiedGpas.erase(gpa);
        }

        get_emugl_vm_operations().mapUserBackedRam(gpa, hva, sizeToPage);

        if (mVerbosePrints) {
            fprintf(stderr, "VERBOSE:%s: registering gpa 0x%llx to mOccupiedGpas\n", __func__,
                    (unsigned long long)gpa);
        }

        mOccupiedGpas[gpa] = {
            vk, device, memory, gpa, sizeToPage,
        };

        if (!mUseOldMemoryCleanupPath) {
            get_emugl_address_space_device_control_ops().register_deallocation_callback(
                this, gpa, [](void* thisPtr, uint64_t gpa) {
                    Impl* implPtr = (Impl*)thisPtr;
                    implPtr->unmapMemoryAtGpaIfExists(gpa);
                });
        }

        return true;
    }

    // Only call this from the address space device deallocation operation's
    // context, or it's possible that the guest/host view of which gpa's are
    // occupied goes out of sync.
    void unmapMemoryAtGpaIfExists(uint64_t gpa) {
        AutoLock lock(mOccupiedGpasLock);

        if (mVerbosePrints) {
            fprintf(stderr, "VERBOSE:%s: deallocation callback for gpa 0x%llx\n", __func__,
                    (unsigned long long)gpa);
        }

        auto* existingMemoryInfo = android::base::find(mOccupiedGpas, gpa);
        if (!existingMemoryInfo) return;

        get_emugl_vm_operations().unmapUserBackedRam(existingMemoryInfo->gpa,
                                                     existingMemoryInfo->sizeToPage);

        mOccupiedGpas.erase(gpa);
    }

    VkResult on_vkAllocateMemory(android::base::BumpPool* pool, VkDevice boxed_device,
                                 const VkMemoryAllocateInfo* pAllocateInfo,
                                 const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);
        auto* tInfo = RenderThreadInfoVk::get();

        if (!pAllocateInfo) return VK_ERROR_INITIALIZATION_FAILED;

        VkMemoryAllocateInfo localAllocInfo = vk_make_orphan_copy(*pAllocateInfo);
        vk_struct_chain_iterator structChainIter = vk_make_chain_iterator(&localAllocInfo);

        VkMemoryAllocateFlagsInfo allocFlagsInfo;
        const VkMemoryAllocateFlagsInfo* allocFlagsInfoPtr =
            vk_find_struct<VkMemoryAllocateFlagsInfo>(pAllocateInfo);
        if (allocFlagsInfoPtr) {
            allocFlagsInfo = *allocFlagsInfoPtr;
            vk_append_struct(&structChainIter, &allocFlagsInfo);
        }

        VkMemoryOpaqueCaptureAddressAllocateInfo opaqueCaptureAddressAllocInfo;
        const VkMemoryOpaqueCaptureAddressAllocateInfo* opaqueCaptureAddressAllocInfoPtr =
            vk_find_struct<VkMemoryOpaqueCaptureAddressAllocateInfo>(pAllocateInfo);
        if (opaqueCaptureAddressAllocInfoPtr) {
            opaqueCaptureAddressAllocInfo = *opaqueCaptureAddressAllocInfoPtr;
            vk_append_struct(&structChainIter, &opaqueCaptureAddressAllocInfo);
        }

        const VkMemoryDedicatedAllocateInfo* dedicatedAllocInfoPtr =
            vk_find_struct<VkMemoryDedicatedAllocateInfo>(pAllocateInfo);
        VkMemoryDedicatedAllocateInfo localDedicatedAllocInfo;

        if (dedicatedAllocInfoPtr) {
            localDedicatedAllocInfo = vk_make_orphan_copy(*dedicatedAllocInfoPtr);
        }
        if (!usingDirectMapping()) {
            // We copy bytes 1 page at a time from the guest to the host
            // if we are not using direct mapping. This means we can end up
            // writing over memory we did not intend.
            // E.g. swiftshader just allocated with malloc, which can have
            // data stored between allocations.
        #ifdef PAGE_SIZE
            localAllocInfo.allocationSize += static_cast<VkDeviceSize>(PAGE_SIZE);
            localAllocInfo.allocationSize &= ~static_cast<VkDeviceSize>(PAGE_SIZE - 1);
        #elif defined(_WIN32)
            localAllocInfo.allocationSize += static_cast<VkDeviceSize>(4096);
            localAllocInfo.allocationSize &= ~static_cast<VkDeviceSize>(4095);
        #else
            localAllocInfo.allocationSize += static_cast<VkDeviceSize>(getpagesize());
            localAllocInfo.allocationSize &= ~static_cast<VkDeviceSize>(getpagesize() - 1);
        #endif
        }
        // Note for AHardwareBuffers, the Vulkan spec states:
        //
        //     Android hardware buffers have intrinsic width, height, format, and usage
        //     properties, so Vulkan images bound to memory imported from an Android
        //     hardware buffer must use dedicated allocations
        //
        // so any allocation requests with a VkImportAndroidHardwareBufferInfoANDROID
        // will necessarily have a VkMemoryDedicatedAllocateInfo. However, the host
        // may or may not actually use a dedicated allocations during Buffer/ColorBuffer
        // setup. Below checks if the underlying Buffer/ColorBuffer backing memory was
        // originally created with a dedicated allocation.
        bool shouldUseDedicatedAllocInfo = dedicatedAllocInfoPtr != nullptr;

        const VkImportColorBufferGOOGLE* importCbInfoPtr =
            vk_find_struct<VkImportColorBufferGOOGLE>(pAllocateInfo);
        const VkImportBufferGOOGLE* importBufferInfoPtr =
            vk_find_struct<VkImportBufferGOOGLE>(pAllocateInfo);

        const VkCreateBlobGOOGLE* createBlobInfoPtr =
            vk_find_struct<VkCreateBlobGOOGLE>(pAllocateInfo);

#ifdef _WIN32
        VkImportMemoryWin32HandleInfoKHR importInfo{
            VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
            0,
            VK_EXT_MEMORY_HANDLE_TYPE_BIT,
            VK_EXT_MEMORY_HANDLE_INVALID,
            L"",
        };
#elif defined(__QNX__)
        VkImportScreenBufferInfoQNX importInfo{
            VK_STRUCTURE_TYPE_IMPORT_SCREEN_BUFFER_INFO_QNX,
            0,
            VK_EXT_MEMORY_HANDLE_INVALID,
        };
#else
        VkImportMemoryFdInfoKHR importInfo{
            VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            0,
            VK_EXT_MEMORY_HANDLE_TYPE_BIT,
            VK_EXT_MEMORY_HANDLE_INVALID,
        };
#endif

#if defined(__APPLE__)
        VkImportMetalBufferInfoEXT importInfoMetalBuffer = {
            VK_STRUCTURE_TYPE_IMPORT_METAL_BUFFER_INFO_EXT,
            0,
            nullptr,
        };
#endif

        void* mappedPtr = nullptr;
        ManagedDescriptor externalMemoryHandle;
        if (importCbInfoPtr) {
            bool colorBufferMemoryUsesDedicatedAlloc = false;
            if (!getColorBufferAllocationInfo(importCbInfoPtr->colorBuffer,
                                              &localAllocInfo.allocationSize,
                                              &localAllocInfo.memoryTypeIndex,
                                              &colorBufferMemoryUsesDedicatedAlloc, &mappedPtr)) {
                if (mSnapshotState != SnapshotState::Loading) {
                    GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                        << "Failed to get allocation info for ColorBuffer:"
                        << importCbInfoPtr->colorBuffer;
                }
                // During snapshot load there could be invalidated references to
                // color buffers.
                // Here we just create a placeholder for it, as it is not suppoed
                // to be used.
                importCbInfoPtr = nullptr;
            } else {
                shouldUseDedicatedAllocInfo &= colorBufferMemoryUsesDedicatedAlloc;

                if (!m_emu->features.GuestVulkanOnly.enabled) {
                    m_emu->callbacks.invalidateColorBuffer(importCbInfoPtr->colorBuffer);
                }

#if defined(__APPLE__)
                // Use metal object extension on MoltenVK mode for color buffer import,
                // non-moltenVK path on MacOS will use FD handles
                if (m_emu->instanceSupportsMoltenVK) {
                    // TODO(b/333460957): This is a temporary fix to get MoltenVK image memory
                    // binding checks working as expected  based on dedicated memory checks. It's
                    // not a valid usage of Vulkan as the device of the image is different than
                    // what's being used here
                    localDedicatedAllocInfo = {
                        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                        .pNext = nullptr,
                        .image = getColorBufferVkImage(importCbInfoPtr->colorBuffer),
                        .buffer = VK_NULL_HANDLE,
                    };
                    shouldUseDedicatedAllocInfo = true;

                    MTLBufferRef cbExtMemoryHandle =
                        getColorBufferMetalMemoryHandle(importCbInfoPtr->colorBuffer);

                    if (cbExtMemoryHandle == nullptr) {
                        fprintf(stderr,
                                "%s: VK_ERROR_OUT_OF_DEVICE_MEMORY: "
                                "colorBuffer 0x%x does not have Vulkan external memory backing\n",
                                __func__, importCbInfoPtr->colorBuffer);
                        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                    }

                    importInfoMetalBuffer.mtlBuffer = cbExtMemoryHandle;
                    vk_append_struct(&structChainIter, &importInfoMetalBuffer);
                } else
#endif
                if (m_emu->deviceInfo.supportsExternalMemoryImport) {
                    VK_EXT_MEMORY_HANDLE cbExtMemoryHandle =
                        getColorBufferExtMemoryHandle(importCbInfoPtr->colorBuffer);

                    if (cbExtMemoryHandle == VK_EXT_MEMORY_HANDLE_INVALID) {
                        fprintf(stderr,
                                "%s: VK_ERROR_OUT_OF_DEVICE_MEMORY: "
                                "colorBuffer 0x%x does not have Vulkan external memory backing\n",
                                __func__, importCbInfoPtr->colorBuffer);
                        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                    }

#if defined(__QNX__)
                    importInfo.buffer = cbExtMemoryHandle;
#else
                    externalMemoryHandle = ManagedDescriptor(dupExternalMemory(cbExtMemoryHandle));

#ifdef _WIN32
                    importInfo.handle =
                        externalMemoryHandle.get().value_or(static_cast<HANDLE>(NULL));
#else
                    importInfo.fd = externalMemoryHandle.get().value_or(-1);
#endif
#endif
                    vk_append_struct(&structChainIter, &importInfo);
                }
            }
        } else if (importBufferInfoPtr) {
            bool bufferMemoryUsesDedicatedAlloc = false;
            if (!getBufferAllocationInfo(
                    importBufferInfoPtr->buffer, &localAllocInfo.allocationSize,
                    &localAllocInfo.memoryTypeIndex, &bufferMemoryUsesDedicatedAlloc)) {
                ERR("Failed to get Buffer:%d allocation info.", importBufferInfoPtr->buffer);
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }

            shouldUseDedicatedAllocInfo &= bufferMemoryUsesDedicatedAlloc;

#ifdef __APPLE__
            if (m_emu->instanceSupportsMoltenVK) {
                MTLBufferRef bufferMetalMemoryHandle =
                    getBufferMetalMemoryHandle(importBufferInfoPtr->buffer);

                if (bufferMetalMemoryHandle == nullptr) {
                    fprintf(stderr,
                            "%s: VK_ERROR_OUT_OF_DEVICE_MEMORY: "
                            "buffer 0x%x does not have Vulkan external memory "
                            "backing\n",
                            __func__, importBufferInfoPtr->buffer);
                    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                }

                importInfoMetalBuffer.mtlBuffer = bufferMetalMemoryHandle;
                vk_append_struct(&structChainIter, &importInfoMetalBuffer);
            } else
#endif
            if (m_emu->deviceInfo.supportsExternalMemoryImport) {
                uint32_t outStreamHandleType;
                VK_EXT_MEMORY_HANDLE bufferExtMemoryHandle =
                    getBufferExtMemoryHandle(importBufferInfoPtr->buffer, &outStreamHandleType);

                if (bufferExtMemoryHandle == VK_EXT_MEMORY_HANDLE_INVALID) {
                    fprintf(stderr,
                            "%s: VK_ERROR_OUT_OF_DEVICE_MEMORY: "
                            "buffer 0x%x does not have Vulkan external memory "
                            "backing\n",
                            __func__, importBufferInfoPtr->buffer);
                    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                }

#if defined(__QNX__)
                importInfo.buffer = bufferExtMemoryHandle;
#else
                bufferExtMemoryHandle = dupExternalMemory(bufferExtMemoryHandle);

#ifdef _WIN32
                importInfo.handle = bufferExtMemoryHandle;
#else
                importInfo.fd = bufferExtMemoryHandle;
#endif
#endif
                vk_append_struct(&structChainIter, &importInfo);
            }
        }

        VkMemoryPropertyFlags memoryPropertyFlags;

        // Map guest memory index to host memory index and lookup memory properties:
        {
            std::lock_guard<std::recursive_mutex> lock(mLock);

            auto* physicalDevice = android::base::find(mDeviceToPhysicalDevice, device);
            if (!physicalDevice) {
                // User app gave an invalid VkDevice, but we don't really want to crash here.
                // We should allow invalid apps.
                return VK_ERROR_DEVICE_LOST;
            }
            auto* physicalDeviceInfo = android::base::find(mPhysdevInfo, *physicalDevice);
            if (!physicalDeviceInfo) {
                GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                    << "No physical device info available for " << *physicalDevice;
            }

            const auto hostMemoryInfoOpt =
                physicalDeviceInfo->memoryPropertiesHelper
                    ->getHostMemoryInfoFromGuestMemoryTypeIndex(localAllocInfo.memoryTypeIndex);
            if (!hostMemoryInfoOpt) {
                return VK_ERROR_INCOMPATIBLE_DRIVER;
            }
            const auto& hostMemoryInfo = *hostMemoryInfoOpt;

            localAllocInfo.memoryTypeIndex = hostMemoryInfo.index;
            memoryPropertyFlags = hostMemoryInfo.memoryType.propertyFlags;
        }

        if (shouldUseDedicatedAllocInfo) {
            vk_append_struct(&structChainIter, &localDedicatedAllocInfo);
        }

        const bool hostVisible = memoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

        if (createBlobInfoPtr && createBlobInfoPtr->blobMem == STREAM_BLOB_MEM_GUEST &&
            (createBlobInfoPtr->blobFlags & STREAM_BLOB_FLAG_CREATE_GUEST_HANDLE)) {
            DescriptorType rawDescriptor;
            uint32_t ctx_id = mSnapshotState == SnapshotState::Loading
                                  ? kTemporaryContextIdForSnapshotLoading
                                  : tInfo->ctx_id;
            auto descriptorInfoOpt = ExternalObjectManager::get()->removeBlobDescriptorInfo(
                ctx_id, createBlobInfoPtr->blobId);
            if (descriptorInfoOpt) {
                auto rawDescriptorOpt = (*descriptorInfoOpt).descriptor.release();
                if (rawDescriptorOpt) {
                    rawDescriptor = *rawDescriptorOpt;
                } else {
                    ERR("Failed vkAllocateMemory: missing raw descriptor.");
                    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                }
            } else {
                ERR("Failed vkAllocateMemory: missing descriptor info.");
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }
#if defined(__linux__)
            importInfo.fd = rawDescriptor;
#endif

#ifdef __linux__
            if (m_emu->deviceInfo.supportsDmaBuf &&
                hasDeviceExtension(device, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
                importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            }
#endif
            vk_append_struct(&structChainIter, &importInfo);
        }

        const bool isImport = importCbInfoPtr || importBufferInfoPtr;
        const bool isExport = !isImport;

        std::optional<VkImportMemoryHostPointerInfoEXT> importHostInfo;
        std::optional<VkExportMemoryAllocateInfo> exportAllocateInfo;

        std::optional<SharedMemory> sharedMemory = std::nullopt;
        std::shared_ptr<PrivateMemory> privateMemory = {};

        if (isExport && hostVisible) {
            if (m_emu->features.SystemBlob.enabled) {
                // Ensure size is page-aligned.
                VkDeviceSize alignedSize = __ALIGN(localAllocInfo.allocationSize, kPageSizeforBlob);
                if (alignedSize != localAllocInfo.allocationSize) {
                    ERR("Warning: Aligning allocation size from %llu to %llu",
                        static_cast<unsigned long long>(localAllocInfo.allocationSize),
                        static_cast<unsigned long long>(alignedSize));
                }
                localAllocInfo.allocationSize = alignedSize;

                static std::atomic<uint64_t> uniqueShmemId = 0;
                sharedMemory = SharedMemory("shared-memory-vk-" + std::to_string(uniqueShmemId++),
                                            localAllocInfo.allocationSize);
                int ret = sharedMemory->create(0600);
                if (ret) {
                    ERR("Failed to create system-blob host-visible memory, error: %d", ret);
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                }
                mappedPtr = sharedMemory->get();
                int mappedPtrAlignment = reinterpret_cast<uintptr_t>(mappedPtr) % kPageSizeforBlob;
                if (mappedPtrAlignment != 0) {
                    ERR("Warning: Mapped shared memory pointer is not aligned to page size, "
                        "alignment "
                        "is: %d",
                        mappedPtrAlignment);
                }
                importHostInfo = {
                    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                    .pNext = NULL,
                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                    .pHostPointer = mappedPtr,
                };
                vk_append_struct(&structChainIter, &*importHostInfo);
            } else if (m_emu->features.ExternalBlob.enabled) {
                VkExternalMemoryHandleTypeFlags handleTypes;

#if defined(__APPLE__)
                if (m_emu->instanceSupportsMoltenVK) {
                    // Using a different handle type when in MoltenVK mode
                    handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_KHR;
                }
#elif defined(_WIN32)
                handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#elif defined(__unix__)
                handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

#ifdef __linux__
                if (m_emu->deviceInfo.supportsDmaBuf &&
                    hasDeviceExtension(device, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
                    handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
                }
#endif

                exportAllocateInfo = {
                    .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                    .pNext = NULL,
                    .handleTypes = handleTypes,
                };
                vk_append_struct(&structChainIter, &*exportAllocateInfo);
            } else if (m_emu->features.VulkanAllocateHostMemory.enabled &&
                       localAllocInfo.pNext == nullptr) {
                if (!m_emu || !m_emu->deviceInfo.supportsExternalMemoryHostProps) {
                    ERR("VK_EXT_EXTERNAL_MEMORY_HOST is not supported, cannot use "
                        "VulkanAllocateHostMemory");
                    return VK_ERROR_INCOMPATIBLE_DRIVER;
                }
                VkDeviceSize alignmentSize =
                    m_emu->deviceInfo.externalMemoryHostProps.minImportedHostPointerAlignment;
                VkDeviceSize alignedSize = __ALIGN(localAllocInfo.allocationSize, alignmentSize);
                localAllocInfo.allocationSize = alignedSize;
                privateMemory =
                    std::make_shared<PrivateMemory>(alignmentSize, localAllocInfo.allocationSize);
                mappedPtr = privateMemory->getAddr();
                importHostInfo = {
                    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                    .pNext = NULL,
                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                    .pHostPointer = mappedPtr,
                };

                VkMemoryHostPointerPropertiesEXT memoryHostPointerProperties = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
                    .pNext = NULL,
                    .memoryTypeBits = 0,
                };

                vk->vkGetMemoryHostPointerPropertiesEXT(
                    device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, mappedPtr,
                    &memoryHostPointerProperties);

                if (memoryHostPointerProperties.memoryTypeBits == 0) {
                    ERR("Cannot find suitable memory type for VulkanAllocateHostMemory");
                    return VK_ERROR_INCOMPATIBLE_DRIVER;
                }

                if (((1u << localAllocInfo.memoryTypeIndex) &
                     memoryHostPointerProperties.memoryTypeBits) == 0) {
                    // TODO Consider assigning the correct memory index earlier, instead of
                    // switching right before allocation.

                    // Look for the first available supported memory index and assign it.
                    for (uint32_t i = 0; i <= 31; ++i) {
                        if ((memoryHostPointerProperties.memoryTypeBits & (1u << i)) == 0) {
                            continue;
                        }
                        localAllocInfo.memoryTypeIndex = i;
                        break;
                    }
                    VERBOSE(
                        "Detected memoryTypeIndex violation on requested host memory import. "
                        "Switching "
                        "to a supported memory index %d",
                        localAllocInfo.memoryTypeIndex);
                }

                vk_append_struct(&structChainIter, &*importHostInfo);
            }
        }

        VkResult result = vk->vkAllocateMemory(device, &localAllocInfo, pAllocator, pMemory);
        if (result != VK_SUCCESS) {
            return result;
        }

#ifdef _WIN32
        // Let ManagedDescriptor to close the underlying HANDLE when going out of scope. From the
        // VkImportMemoryWin32HandleInfoKHR spec: Importing memory object payloads from Windows
        // handles does not transfer ownership of the handle to the Vulkan implementation. For
        // handle types defined as NT handles, the application must release handle ownership using
        // the CloseHandle system call when the handle is no longer needed. For handle types defined
        // as NT handles, the imported memory object holds a reference to its payload.
#else
        // Tell ManagedDescriptor not to close the underlying fd, because the ownership has already
        // been transferred to the Vulkan implementation. From VkImportMemoryFdInfoKHR spec:
        // Importing memory from a file descriptor transfers ownership of the file descriptor from
        // the application to the Vulkan implementation. The application must not perform any
        // operations on the file descriptor after a successful import. The imported memory object
        // holds a reference to its payload.
        externalMemoryHandle.release();
#endif

        std::lock_guard<std::recursive_mutex> lock(mLock);

        mMemoryInfo[*pMemory] = MemoryInfo();
        auto& memoryInfo = mMemoryInfo[*pMemory];
        memoryInfo.size = localAllocInfo.allocationSize;
        memoryInfo.device = device;
        memoryInfo.memoryIndex = localAllocInfo.memoryTypeIndex;

        if (importCbInfoPtr) {
            memoryInfo.boundColorBuffer = importCbInfoPtr->colorBuffer;
        }

        if (!hostVisible) {
            *pMemory = new_boxed_non_dispatchable_VkDeviceMemory(*pMemory);
            return result;
        }

        if (memoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
            memoryInfo.caching = MAP_CACHE_CACHED;
        } else if (memoryPropertyFlags & VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD) {
            memoryInfo.caching = MAP_CACHE_UNCACHED;
        } else if (memoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
            memoryInfo.caching = MAP_CACHE_WC;
        }

        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;

        // If gfxstream needs to be able to read from this memory, needToMap should be true.
        // When external blobs are off, we always want to map HOST_VISIBLE memory. Because, we run
        // in the same process as the guest.
        // When external blobs are on, we want to map memory only if a workaround is using it in
        // the gfxstream process. This happens when ASTC CPU emulation is on.
        bool needToMap =
            (!m_emu->features.ExternalBlob.enabled ||
             (deviceInfo->useAstcCpuDecompression && deviceInfo->emulateTextureAstc)) &&
            !createBlobInfoPtr;

        // Some cases provide a mappedPtr, so we only map if we still don't have a pointer here.
        if (!mappedPtr && needToMap) {
            memoryInfo.needUnmap = true;
            VkResult mapResult =
                vk->vkMapMemory(device, *pMemory, 0, memoryInfo.size, 0, &memoryInfo.ptr);
            if (mapResult != VK_SUCCESS) {
                freeMemoryLocked(vk, device, *pMemory, pAllocator);
                *pMemory = VK_NULL_HANDLE;
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        } else {
            // Since we didn't call vkMapMemory, unmapping is not needed (don't own mappedPtr).
            memoryInfo.needUnmap = false;
            memoryInfo.ptr = mappedPtr;

            if (createBlobInfoPtr) {
                memoryInfo.blobId = createBlobInfoPtr->blobId;
            }

            // Always assign the shared memory into memoryInfo. If it was used, then it will have
            // ownership transferred.
            memoryInfo.sharedMemory = std::exchange(sharedMemory, std::nullopt);

            memoryInfo.privateMemory = privateMemory;
        }

        *pMemory = new_boxed_non_dispatchable_VkDeviceMemory(*pMemory);

        return result;
    }

    void freeMemoryLocked(VulkanDispatch* vk, VkDevice device, VkDeviceMemory memory,
                          const VkAllocationCallbacks* pAllocator) {
        auto* info = android::base::find(mMemoryInfo, memory);
        if (!info) return;  // Invalid usage.

        if (info->directMapped) {
            // if direct mapped, we leave it up to the guest address space driver
            // to control the unmapping of kvm slot on the host side
            // in order to avoid situations where
            //
            // 1. we try to unmap here and deadlock
            //
            // 2. unmapping at the wrong time (possibility of a parallel call
            // to unmap vs. address space allocate and mapMemory leading to
            // mapping the same gpa twice)
            if (mUseOldMemoryCleanupPath) {
                unmapMemoryAtGpaIfExists(info->guestPhysAddr);
            }
        }

        if (info->virtioGpuMapped) {
            if (mLogging) {
                fprintf(stderr, "%s: unmap hostmem %p id 0x%llx\n", __func__, info->ptr,
                        (unsigned long long)info->hostmemId);
            }
        }

        if (info->needUnmap && info->ptr) {
            vk->vkUnmapMemory(device, memory);
        }

        vk->vkFreeMemory(device, memory, pAllocator);

        mMemoryInfo.erase(memory);
    }

    void on_vkFreeMemory(android::base::BumpPool* pool, VkDevice boxed_device,
                         VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        if (!device || !vk) {
            return;
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);

        freeMemoryLocked(vk, device, memory, pAllocator);
    }

    VkResult on_vkMapMemory(android::base::BumpPool* pool, VkDevice, VkDeviceMemory memory,
                            VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags,
                            void** ppData) {
        std::lock_guard<std::recursive_mutex> lock(mLock);
        return on_vkMapMemoryLocked(0, memory, offset, size, flags, ppData);
    }
    VkResult on_vkMapMemoryLocked(VkDevice, VkDeviceMemory memory, VkDeviceSize offset,
                                  VkDeviceSize size, VkMemoryMapFlags flags, void** ppData) {
        auto* info = android::base::find(mMemoryInfo, memory);
        if (!info || !info->ptr) return VK_ERROR_MEMORY_MAP_FAILED;  // Invalid usage.

        *ppData = (void*)((uint8_t*)info->ptr + offset);
        return VK_SUCCESS;
    }

    void on_vkUnmapMemory(android::base::BumpPool* pool, VkDevice, VkDeviceMemory) {
        // no-op; user-level mapping does not correspond
        // to any operation here.
    }

    uint8_t* getMappedHostPointer(VkDeviceMemory memory) {
        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* info = android::base::find(mMemoryInfo, memory);
        if (!info) return nullptr;

        return (uint8_t*)(info->ptr);
    }

    VkDeviceSize getDeviceMemorySize(VkDeviceMemory memory) {
        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* info = android::base::find(mMemoryInfo, memory);
        if (!info) return 0;

        return info->size;
    }

    bool usingDirectMapping() const {
        return m_emu->features.GlDirectMem.enabled ||
               m_emu->features.VirtioGpuNext.enabled;
    }

    HostFeatureSupport getHostFeatureSupport() const {
        HostFeatureSupport res;

        if (!m_vk) return res;

        auto emu = getGlobalVkEmulation();

        res.supportsVulkan = emu && emu->live;

        if (!res.supportsVulkan) return res;

        const auto& props = emu->deviceInfo.physdevProps;

        res.supportsVulkan1_1 = props.apiVersion >= VK_API_VERSION_1_1;
        res.useDeferredCommands = emu->useDeferredCommands;
        res.useCreateResourcesWithRequirements = emu->useCreateResourcesWithRequirements;

        res.apiVersion = props.apiVersion;
        res.driverVersion = props.driverVersion;
        res.deviceID = props.deviceID;
        res.vendorID = props.vendorID;
        return res;
    }

    bool hasInstanceExtension(VkInstance instance, const std::string& name) {
        auto* info = android::base::find(mInstanceInfo, instance);
        if (!info) return false;

        for (const auto& enabledName : info->enabledExtensionNames) {
            if (name == enabledName) return true;
        }

        return false;
    }

    bool hasDeviceExtension(VkDevice device, const std::string& name) {
        auto* info = android::base::find(mDeviceInfo, device);
        if (!info) return false;

        for (const auto& enabledName : info->enabledExtensionNames) {
            if (name == enabledName) return true;
        }

        return false;
    }

    // Returns whether a vector of VkExtensionProperties contains a particular extension
    bool hasDeviceExtension(const std::vector<VkExtensionProperties>& properties,
                            const char* name) {
        for (const auto& prop : properties) {
            if (strcmp(prop.extensionName, name) == 0) return true;
        }
        return false;
    }

    // Convenience function to call vkEnumerateDeviceExtensionProperties and get the results as an
    // std::vector
    VkResult enumerateDeviceExtensionProperties(VulkanDispatch* vk, VkPhysicalDevice physicalDevice,
                                                const char* pLayerName,
                                                std::vector<VkExtensionProperties>& properties) {
        uint32_t propertyCount = 0;
        VkResult result = vk->vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName,
                                                                   &propertyCount, nullptr);
        if (result != VK_SUCCESS) return result;

        properties.resize(propertyCount);
        return vk->vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &propertyCount,
                                                        properties.data());
    }

    // VK_ANDROID_native_buffer
    VkResult on_vkGetSwapchainGrallocUsageANDROID(android::base::BumpPool* pool, VkDevice,
                                                  VkFormat format, VkImageUsageFlags imageUsage,
                                                  int* grallocUsage) {
        getGralloc0Usage(format, imageUsage, grallocUsage);
        return VK_SUCCESS;
    }

    VkResult on_vkGetSwapchainGrallocUsage2ANDROID(
        android::base::BumpPool* pool, VkDevice, VkFormat format, VkImageUsageFlags imageUsage,
        VkSwapchainImageUsageFlagsANDROID swapchainImageUsage, uint64_t* grallocConsumerUsage,
        uint64_t* grallocProducerUsage) {
        getGralloc1Usage(format, imageUsage, swapchainImageUsage, grallocConsumerUsage,
                         grallocProducerUsage);
        return VK_SUCCESS;
    }

    VkResult on_vkAcquireImageANDROID(android::base::BumpPool* pool, VkDevice boxed_device,
                                      VkImage image, int nativeFenceFd, VkSemaphore semaphore,
                                      VkFence fence) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return VK_ERROR_INITIALIZATION_FAILED;

        auto* imageInfo = android::base::find(mImageInfo, image);
        if (!imageInfo) return VK_ERROR_INITIALIZATION_FAILED;

        VkQueue defaultQueue;
        uint32_t defaultQueueFamilyIndex;
        Lock* defaultQueueLock;
        if (!getDefaultQueueForDeviceLocked(device, &defaultQueue, &defaultQueueFamilyIndex,
                                            &defaultQueueLock)) {
            fprintf(stderr, "%s: cant get the default q\n", __func__);
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        DeviceOpBuilder builder(*deviceInfo->deviceOpTracker);

        VkFence usedFence = fence;
        if (usedFence == VK_NULL_HANDLE) {
            usedFence = builder.CreateFenceForOp();
        }

        AndroidNativeBufferInfo* anbInfo = imageInfo->anbInfo.get();

        VkResult result = setAndroidNativeImageSemaphoreSignaled(
            vk, device, defaultQueue, defaultQueueFamilyIndex, defaultQueueLock, semaphore,
            usedFence, anbInfo);
        if (result != VK_SUCCESS) {
            return result;
        }

        DeviceOpWaitable aniCompletedWaitable = builder.OnQueueSubmittedWithFence(usedFence);

        if (semaphore != VK_NULL_HANDLE) {
            auto semaphoreInfo = android::base::find(mSemaphoreInfo, semaphore);
            if (semaphoreInfo != nullptr) {
                semaphoreInfo->latestUse = aniCompletedWaitable;
            }
        }
        if (fence != VK_NULL_HANDLE) {
            auto fenceInfo = android::base::find(mFenceInfo, fence);
            if (fenceInfo != nullptr) {
                fenceInfo->latestUse = aniCompletedWaitable;
            }
        }

        deviceInfo->deviceOpTracker->PollAndProcessGarbage();

        return VK_SUCCESS;
    }

    VkResult on_vkQueueSignalReleaseImageANDROID(android::base::BumpPool* pool, VkQueue boxed_queue,
                                                 uint32_t waitSemaphoreCount,
                                                 const VkSemaphore* pWaitSemaphores, VkImage image,
                                                 int* pNativeFenceFd) {
        auto queue = unbox_VkQueue(boxed_queue);
        auto vk = dispatch_VkQueue(boxed_queue);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* queueInfo = android::base::find(mQueueInfo, queue);
        if (!queueInfo) return VK_ERROR_INITIALIZATION_FAILED;

        if (mRenderDocWithMultipleVkInstances) {
            VkPhysicalDevice vkPhysicalDevice = mDeviceToPhysicalDevice.at(queueInfo->device);
            VkInstance vkInstance = mPhysicalDeviceToInstance.at(vkPhysicalDevice);
            mRenderDocWithMultipleVkInstances->onFrameDelimiter(vkInstance);
        }

        auto* imageInfo = android::base::find(mImageInfo, image);
        auto anbInfo = imageInfo->anbInfo;

        if (anbInfo->useVulkanNativeImage) {
            // vkQueueSignalReleaseImageANDROID() is only called by the Android framework's
            // implementation of vkQueuePresentKHR(). The guest application is responsible for
            // transitioning the image layout of the image passed to vkQueuePresentKHR() to
            // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR before the call. If the host is using native
            // Vulkan images where `image` is backed with the same memory as its ColorBuffer,
            // then we need to update the tracked layout for that ColorBuffer.
            setColorBufferCurrentLayout(anbInfo->colorBufferHandle,
                                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }

        return syncImageToColorBuffer(m_emu->callbacks, vk, queueInfo->queueFamilyIndex, queue,
                                      queueInfo->lock, waitSemaphoreCount, pWaitSemaphores,
                                      pNativeFenceFd, anbInfo);
    }

    VkResult on_vkMapMemoryIntoAddressSpaceGOOGLE(android::base::BumpPool* pool,
                                                  VkDevice boxed_device, VkDeviceMemory memory,
                                                  uint64_t* pAddress) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        if (!m_emu->features.GlDirectMem.enabled) {
            fprintf(stderr,
                    "FATAL: Tried to use direct mapping "
                    "while GlDirectMem is not enabled!\n");
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);

        if (mLogging) {
            fprintf(stderr, "%s: deviceMemory: 0x%llx pAddress: 0x%llx\n", __func__,
                    (unsigned long long)memory, (unsigned long long)(*pAddress));
        }

        if (!mapHostVisibleMemoryToGuestPhysicalAddressLocked(vk, device, memory, *pAddress)) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        auto* info = android::base::find(mMemoryInfo, memory);
        if (!info) return VK_ERROR_INITIALIZATION_FAILED;

        *pAddress = (uint64_t)(uintptr_t)info->ptr;

        return VK_SUCCESS;
    }

    VkResult vkGetBlobInternal(VkDevice boxed_device, VkDeviceMemory memory, uint64_t hostBlobId) {
        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* tInfo = RenderThreadInfoVk::get();
        uint32_t ctx_id = mSnapshotState == SnapshotState::Loading
                              ? kTemporaryContextIdForSnapshotLoading
                              : tInfo->ctx_id;

        auto* info = android::base::find(mMemoryInfo, memory);
        if (!info) return VK_ERROR_OUT_OF_HOST_MEMORY;

        hostBlobId = (info->blobId && !hostBlobId) ? info->blobId : hostBlobId;

        if (m_emu->features.SystemBlob.enabled && info->sharedMemory.has_value()) {
            uint32_t handleType = STREAM_MEM_HANDLE_TYPE_SHM;
            // We transfer ownership of the shared memory handle to the descriptor info.
            // The memory itself is destroyed only when all processes unmap / release their
            // handles.
            ExternalObjectManager::get()->addBlobDescriptorInfo(
                ctx_id, hostBlobId, info->sharedMemory->releaseHandle(), handleType, info->caching,
                std::nullopt);
        } else if (m_emu->features.ExternalBlob.enabled) {
            VkResult result;
            auto device = unbox_VkDevice(boxed_device);
            auto vk = dispatch_VkDevice(boxed_device);
            DescriptorType handle;
            uint32_t handleType;
            struct VulkanInfo vulkanInfo = {
                .memoryIndex = info->memoryIndex,
            };
            memcpy(vulkanInfo.deviceUUID, m_emu->deviceInfo.idProps.deviceUUID,
                   sizeof(vulkanInfo.deviceUUID));
            memcpy(vulkanInfo.driverUUID, m_emu->deviceInfo.idProps.driverUUID,
                   sizeof(vulkanInfo.driverUUID));

            if (snapshotsEnabled()) {
                VkResult mapResult = vk->vkMapMemory(device, memory, 0, info->size, 0, &info->ptr);
                if (mapResult != VK_SUCCESS) {
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                }

                info->needUnmap = true;
            }

#ifdef __unix__
            VkMemoryGetFdInfoKHR getFd = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                .pNext = nullptr,
                .memory = memory,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
            };

            handleType = STREAM_MEM_HANDLE_TYPE_OPAQUE_FD;
#endif

#ifdef __linux__
            if (m_emu->deviceInfo.supportsDmaBuf &&
                hasDeviceExtension(device, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
                getFd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
                handleType = STREAM_MEM_HANDLE_TYPE_DMABUF;
            }
#endif

#ifdef __unix__
            result = m_emu->deviceInfo.getMemoryHandleFunc(device, &getFd, &handle);
            if (result != VK_SUCCESS) {
                return result;
            }
#endif

#ifdef _WIN32
            VkMemoryGetWin32HandleInfoKHR getHandle = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
                .pNext = nullptr,
                .memory = memory,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
            };

            handleType = STREAM_MEM_HANDLE_TYPE_OPAQUE_WIN32;

            result = m_emu->deviceInfo.getMemoryHandleFunc(device, &getHandle, &handle);
            if (result != VK_SUCCESS) {
                return result;
            }
#endif

#ifdef __APPLE__
            if (m_emu->instanceSupportsMoltenVK) {
                GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                    << "ExternalBlob feature is not supported with MoltenVK";
            }
#endif

            ManagedDescriptor managedHandle(handle);
            ExternalObjectManager::get()->addBlobDescriptorInfo(
                ctx_id, hostBlobId, std::move(managedHandle), handleType, info->caching,
                std::optional<VulkanInfo>(vulkanInfo));
        } else if (!info->needUnmap) {
            auto device = unbox_VkDevice(boxed_device);
            auto vk = dispatch_VkDevice(boxed_device);
            VkResult mapResult = vk->vkMapMemory(device, memory, 0, info->size, 0, &info->ptr);
            if (mapResult != VK_SUCCESS) {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }

            info->needUnmap = true;
        }

        if (info->needUnmap) {
            uint64_t hva = (uint64_t)(uintptr_t)(info->ptr);
            uint64_t alignedHva = hva & kPageMaskForBlob;

            if (hva != alignedHva) {
                ERR("Mapping non page-size (0x%" PRIx64
                    ") aligned host virtual address:%p "
                    "using the aligned host virtual address:%p. The underlying resources "
                    "using this blob may be corrupted/offset.",
                    kPageSizeforBlob, hva, alignedHva);
            }
            ExternalObjectManager::get()->addMapping(ctx_id, hostBlobId,
                                                     (void*)(uintptr_t)alignedHva, info->caching);
            info->virtioGpuMapped = true;
            info->hostmemId = hostBlobId;
        }

        return VK_SUCCESS;
    }

    VkResult on_vkGetBlobGOOGLE(android::base::BumpPool* pool, VkDevice boxed_device,
                                VkDeviceMemory memory) {
        return vkGetBlobInternal(boxed_device, memory, 0);
    }

    VkResult on_vkGetMemoryHostAddressInfoGOOGLE(android::base::BumpPool* pool,
                                                 VkDevice boxed_device, VkDeviceMemory memory,
                                                 uint64_t* pAddress, uint64_t* pSize,
                                                 uint64_t* pHostmemId) {
        hostBlobId++;
        *pHostmemId = hostBlobId;
        return vkGetBlobInternal(boxed_device, memory, hostBlobId);
    }

    VkResult on_vkFreeMemorySyncGOOGLE(android::base::BumpPool* pool, VkDevice boxed_device,
                                       VkDeviceMemory memory,
                                       const VkAllocationCallbacks* pAllocator) {
        on_vkFreeMemory(pool, boxed_device, memory, pAllocator);

        return VK_SUCCESS;
    }

    VkResult on_vkAllocateCommandBuffers(android::base::BumpPool* pool, VkDevice boxed_device,
                                         const VkCommandBufferAllocateInfo* pAllocateInfo,
                                         VkCommandBuffer* pCommandBuffers) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        VkResult result = vk->vkAllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);

        if (result != VK_SUCCESS) {
            return result;
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return VK_ERROR_UNKNOWN;

        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
            mCommandBufferInfo[pCommandBuffers[i]] = CommandBufferInfo();
            mCommandBufferInfo[pCommandBuffers[i]].device = device;
            mCommandBufferInfo[pCommandBuffers[i]].debugUtilsHelper = deviceInfo->debugUtilsHelper;
            mCommandBufferInfo[pCommandBuffers[i]].cmdPool = pAllocateInfo->commandPool;
            auto boxed = new_boxed_VkCommandBuffer(pCommandBuffers[i], vk,
                                                   false /* does not own dispatch */);
            mCommandBufferInfo[pCommandBuffers[i]].boxed = boxed;
            pCommandBuffers[i] = (VkCommandBuffer)boxed;
        }
        return result;
    }

    VkResult on_vkCreateCommandPool(android::base::BumpPool* pool, VkDevice boxed_device,
                                    const VkCommandPoolCreateInfo* pCreateInfo,
                                    const VkAllocationCallbacks* pAllocator,
                                    VkCommandPool* pCommandPool) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        VkResult result = vk->vkCreateCommandPool(device, pCreateInfo, pAllocator, pCommandPool);
        if (result != VK_SUCCESS) {
            return result;
        }
        std::lock_guard<std::recursive_mutex> lock(mLock);
        mCommandPoolInfo[*pCommandPool] = CommandPoolInfo();
        auto& cmdPoolInfo = mCommandPoolInfo[*pCommandPool];
        cmdPoolInfo.device = device;

        *pCommandPool = new_boxed_non_dispatchable_VkCommandPool(*pCommandPool);
        cmdPoolInfo.boxed = *pCommandPool;

        return result;
    }

    void on_vkDestroyCommandPool(android::base::BumpPool* pool, VkDevice boxed_device,
                                 VkCommandPool commandPool,
                                 const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        vk->vkDestroyCommandPool(device, commandPool, pAllocator);
        std::lock_guard<std::recursive_mutex> lock(mLock);
        const auto* cmdPoolInfo = android::base::find(mCommandPoolInfo, commandPool);
        if (cmdPoolInfo) {
            removeCommandBufferInfo(cmdPoolInfo->cmdBuffers);
            mCommandPoolInfo.erase(commandPool);
        }
    }

    VkResult on_vkResetCommandPool(android::base::BumpPool* pool, VkDevice boxed_device,
                                   VkCommandPool commandPool, VkCommandPoolResetFlags flags) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        VkResult result = vk->vkResetCommandPool(device, commandPool, flags);
        if (result != VK_SUCCESS) {
            return result;
        }
        return result;
    }

    void on_vkCmdExecuteCommands(android::base::BumpPool* pool, VkCommandBuffer boxed_commandBuffer,
                                 uint32_t commandBufferCount,
                                 const VkCommandBuffer* pCommandBuffers) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        vk->vkCmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
        std::lock_guard<std::recursive_mutex> lock(mLock);
        CommandBufferInfo& cmdBuffer = mCommandBufferInfo[commandBuffer];
        cmdBuffer.subCmds.insert(cmdBuffer.subCmds.end(), pCommandBuffers,
                                 pCommandBuffers + commandBufferCount);
    }

    VkResult dispatchVkQueueSubmit(VulkanDispatch* vk, VkQueue unboxed_queue, uint32_t submitCount,
                                   const VkSubmitInfo* pSubmits, VkFence fence) {
        return vk->vkQueueSubmit(unboxed_queue, submitCount, pSubmits, fence);
    }

    VkResult dispatchVkQueueSubmit(VulkanDispatch* vk, VkQueue unboxed_queue, uint32_t submitCount,
                                   const VkSubmitInfo2* pSubmits, VkFence fence) {
        return vk->vkQueueSubmit2(unboxed_queue, submitCount, pSubmits, fence);
    }

    int getCommandBufferCount(const VkSubmitInfo& submitInfo) {
        return submitInfo.commandBufferCount;
    }

    VkCommandBuffer getCommandBuffer(const VkSubmitInfo& submitInfo, int idx) {
        return submitInfo.pCommandBuffers[idx];
    }

    int getCommandBufferCount(const VkSubmitInfo2& submitInfo) {
        return submitInfo.commandBufferInfoCount;
    }

    VkCommandBuffer getCommandBuffer(const VkSubmitInfo2& submitInfo, int idx) {
        return submitInfo.pCommandBufferInfos[idx].commandBuffer;
    }

    uint32_t getWaitSemaphoreCount(const VkSubmitInfo& pSubmit) {
        return pSubmit.waitSemaphoreCount;
    }
    uint32_t getWaitSemaphoreCount(const VkSubmitInfo2& pSubmit) {
        return pSubmit.waitSemaphoreInfoCount;
    }
    VkSemaphore getWaitSemaphore(const VkSubmitInfo& pSubmit, int i) {
        return pSubmit.pWaitSemaphores[i];
    }
    VkSemaphore getWaitSemaphore(const VkSubmitInfo2& pSubmit, int i) {
        return pSubmit.pWaitSemaphoreInfos[i].semaphore;
    }

    uint32_t getSignalSemaphoreCount(const VkSubmitInfo& pSubmit) {
        return pSubmit.signalSemaphoreCount;
    }
    uint32_t getSignalSemaphoreCount(const VkSubmitInfo2& pSubmit) {
        return pSubmit.signalSemaphoreInfoCount;
    }
    VkSemaphore getSignalSemaphore(const VkSubmitInfo& pSubmit, int i) {
        return pSubmit.pSignalSemaphores[i];
    }
    VkSemaphore getSignalSemaphore(const VkSubmitInfo2& pSubmit, int i) {
        return pSubmit.pSignalSemaphoreInfos[i].semaphore;
    }

    template <typename VkSubmitInfoType>
    VkResult on_vkQueueSubmit(android::base::BumpPool* pool, VkQueue boxed_queue,
                              uint32_t submitCount, const VkSubmitInfoType* pSubmits,
                              VkFence fence) {
        auto queue = unbox_VkQueue(boxed_queue);
        auto vk = dispatch_VkQueue(boxed_queue);

        std::unordered_set<HandleType> acquiredColorBuffers;
        std::unordered_set<HandleType> releasedColorBuffers;
        if (!m_emu->features.GuestVulkanOnly.enabled) {
            {
                std::lock_guard<std::recursive_mutex> lock(mLock);
                for (int i = 0; i < submitCount; i++) {
                    for (int j = 0; j < getCommandBufferCount(pSubmits[i]); j++) {
                        VkCommandBuffer cmdBuffer = getCommandBuffer(pSubmits[i], j);
                        CommandBufferInfo* cmdBufferInfo =
                            android::base::find(mCommandBufferInfo, cmdBuffer);
                        if (!cmdBufferInfo) {
                            continue;
                        }
                        for (auto descriptorSet : cmdBufferInfo->allDescriptorSets) {
                            auto descriptorSetInfo =
                                android::base::find(mDescriptorSetInfo, descriptorSet);
                            if (!descriptorSetInfo) {
                                continue;
                            }
                            for (auto& writes : descriptorSetInfo->allWrites) {
                                for (const auto& write : writes) {
                                    bool isValid = true;
                                    for (const auto& alive : write.alives) {
                                        isValid &= !alive.expired();
                                    }
                                    if (isValid && write.boundColorBuffer.has_value()) {
                                        acquiredColorBuffers.insert(write.boundColorBuffer.value());
                                    }
                                }
                            }
                        }

                        acquiredColorBuffers.merge(cmdBufferInfo->acquiredColorBuffers);
                        releasedColorBuffers.merge(cmdBufferInfo->releasedColorBuffers);
                        for (const auto& ite : cmdBufferInfo->cbLayouts) {
                            setColorBufferCurrentLayout(ite.first, ite.second);
                        }
                    }
                }
            }

            for (HandleType cb : acquiredColorBuffers) {
                m_emu->callbacks.invalidateColorBuffer(cb);
            }
        }

        VkDevice device = VK_NULL_HANDLE;
        Lock* ql;
        {
            std::lock_guard<std::recursive_mutex> lock(mLock);

            {
                auto* queueInfo = android::base::find(mQueueInfo, queue);
                if (queueInfo) {
                    device = queueInfo->device;
                    // Unsafe to release when snapshot enabled.
                    // Snapshot load might fail to find the shader modules if we release them here.
                    if (!snapshotsEnabled()) {
                        sBoxedHandleManager.processDelayedRemovesGlobalStateLocked(device);
                    }
                }
            }

            for (uint32_t i = 0; i < submitCount; i++) {
                executePreprocessRecursive(pSubmits[i]);
            }

            auto* queueInfo = android::base::find(mQueueInfo, queue);
            if (!queueInfo) return VK_SUCCESS;
            ql = queueInfo->lock;
        }

        VkFence usedFence = fence;
        DeviceOpWaitable queueCompletedWaitable;
        {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            auto* deviceInfo = android::base::find(mDeviceInfo, device);
            if (!deviceInfo) return VK_ERROR_INITIALIZATION_FAILED;
            DeviceOpBuilder builder(*deviceInfo->deviceOpTracker);
            if (VK_NULL_HANDLE == usedFence) {
                // Note: This fence will be managed by the DeviceOpTracker after the
                // OnQueueSubmittedWithFence call, so it does not need to be destroyed in the scope
                // of this queueSubmit
                usedFence = builder.CreateFenceForOp();
            }
            queueCompletedWaitable = builder.OnQueueSubmittedWithFence(usedFence);

            deviceInfo->deviceOpTracker->PollAndProcessGarbage();
        }

        {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            std::unordered_set<HandleType> imageBarrierColorBuffers;
            for (int i = 0; i < submitCount; i++) {
                for (int j = 0; j < getCommandBufferCount(pSubmits[i]); j++) {
                    VkCommandBuffer cmdBuffer = getCommandBuffer(pSubmits[i], j);
                    CommandBufferInfo* cmdBufferInfo =
                        android::base::find(mCommandBufferInfo, cmdBuffer);
                    if (cmdBufferInfo) {
                        imageBarrierColorBuffers.merge(cmdBufferInfo->imageBarrierColorBuffers);
                    }
                }
            }
            auto* deviceInfo = android::base::find(mDeviceInfo, device);
            if (!deviceInfo) return VK_ERROR_INITIALIZATION_FAILED;
            for (const auto& colorBuffer : imageBarrierColorBuffers) {
                setColorBufferLatestUse(colorBuffer, queueCompletedWaitable,
                                        deviceInfo->deviceOpTracker);
            }
        }

        AutoLock qlock(*ql);
        auto result = dispatchVkQueueSubmit(vk, queue, submitCount, pSubmits, usedFence);

        if (result != VK_SUCCESS) {
            WARN("dispatchVkQueueSubmit failed: %s [%d]", string_VkResult(result), result);
            return result;
        }
        {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            // Update image layouts
            for (int i = 0; i < submitCount; i++) {
                for (int j = 0; j < getCommandBufferCount(pSubmits[i]); j++) {
                    VkCommandBuffer cmdBuffer = getCommandBuffer(pSubmits[i], j);
                    CommandBufferInfo* cmdBufferInfo =
                        android::base::find(mCommandBufferInfo, cmdBuffer);
                    if (!cmdBufferInfo) {
                        continue;
                    }
                    for (const auto& ite : cmdBufferInfo->imageLayouts) {
                        auto imageIte = mImageInfo.find(ite.first);
                        if (imageIte == mImageInfo.end()) {
                            continue;
                        }
                        imageIte->second.layout = ite.second;
                    }
                }
            }
            // Update latestUse for all wait/signal semaphores, to ensure that they
            // are never asynchronously destroyed before the queue submissions referencing
            // them have completed
            for (int i = 0; i < submitCount; i++) {
                for (int j = 0; j < getWaitSemaphoreCount(pSubmits[i]); j++) {
                    SemaphoreInfo* semaphoreInfo =
                        android::base::find(mSemaphoreInfo, getWaitSemaphore(pSubmits[i], j));
                    if (semaphoreInfo) {
                        semaphoreInfo->latestUse = queueCompletedWaitable;
                    }
                }
                for (int j = 0; j < getSignalSemaphoreCount(pSubmits[i]); j++) {
                    SemaphoreInfo* semaphoreInfo =
                        android::base::find(mSemaphoreInfo, getSignalSemaphore(pSubmits[i], j));
                    if (semaphoreInfo) {
                        semaphoreInfo->latestUse = queueCompletedWaitable;
                    }
                }
            }

            // After vkQueueSubmit is called, we can signal the conditional variable
            // in FenceInfo, so that other threads (e.g. SyncThread) can call
            // waitForFence() on this fence.
            auto* fenceInfo = android::base::find(mFenceInfo, fence);
            if (fenceInfo) {
                fenceInfo->state = FenceInfo::State::kWaitable;
                fenceInfo->lock.lock();
                fenceInfo->cv.signalAndUnlock(&fenceInfo->lock);
                // Also update the latestUse waitable for this fence, to ensure
                // it is not asynchronously destroyed before all the waitables
                // referencing it
                fenceInfo->latestUse = queueCompletedWaitable;
            }
        }
        if (!releasedColorBuffers.empty()) {
            vk->vkWaitForFences(device, 1, &usedFence, VK_TRUE, /* 1 sec */ 1000000000L);

            for (HandleType cb : releasedColorBuffers) {
                m_emu->callbacks.flushColorBuffer(cb);
            }
        }

        return result;
    }

    VkResult on_vkQueueWaitIdle(android::base::BumpPool* pool, VkQueue boxed_queue) {
        auto queue = unbox_VkQueue(boxed_queue);
        auto vk = dispatch_VkQueue(boxed_queue);

        if (!queue) return VK_SUCCESS;

        Lock* ql;
        {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            auto* queueInfo = android::base::find(mQueueInfo, queue);
            if (!queueInfo) return VK_SUCCESS;
            ql = queueInfo->lock;
        }

        AutoLock qlock(*ql);
        return vk->vkQueueWaitIdle(queue);
    }

    VkResult on_vkResetCommandBuffer(android::base::BumpPool* pool,
                                     VkCommandBuffer boxed_commandBuffer,
                                     VkCommandBufferResetFlags flags) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        m_emu->deviceLostHelper.onResetCommandBuffer(commandBuffer);

        VkResult result = vk->vkResetCommandBuffer(commandBuffer, flags);
        if (VK_SUCCESS == result) {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            auto& bufferInfo = mCommandBufferInfo[commandBuffer];
            bufferInfo.reset();
        }
        return result;
    }

    void on_vkFreeCommandBuffers(android::base::BumpPool* pool, VkDevice boxed_device,
                                 VkCommandPool commandPool, uint32_t commandBufferCount,
                                 const VkCommandBuffer* pCommandBuffers) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        if (!device) return;

        for (uint32_t i = 0; i < commandBufferCount; i++) {
            m_emu->deviceLostHelper.onFreeCommandBuffer(pCommandBuffers[i]);
        }

        vk->vkFreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        for (uint32_t i = 0; i < commandBufferCount; i++) {
            const auto& cmdBufferInfoIt = mCommandBufferInfo.find(pCommandBuffers[i]);
            if (cmdBufferInfoIt != mCommandBufferInfo.end()) {
                const auto& cmdPoolInfoIt = mCommandPoolInfo.find(cmdBufferInfoIt->second.cmdPool);
                if (cmdPoolInfoIt != mCommandPoolInfo.end()) {
                    cmdPoolInfoIt->second.cmdBuffers.erase(pCommandBuffers[i]);
                }
                // Done in decoder
                // delete_VkCommandBuffer(cmdBufferInfoIt->second.boxed);
                mCommandBufferInfo.erase(cmdBufferInfoIt);
            }
        }
    }

    void on_vkGetPhysicalDeviceExternalSemaphoreProperties(
        android::base::BumpPool* pool, VkPhysicalDevice boxed_physicalDevice,
        const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo,
        VkExternalSemaphoreProperties* pExternalSemaphoreProperties) {
        auto physicalDevice = unbox_VkPhysicalDevice(boxed_physicalDevice);

        if (!physicalDevice) {
            return;
        }

        if (m_emu->features.VulkanExternalSync.enabled) {
            // Cannot forward this call to driver because nVidia linux driver crahses on it.
            switch (pExternalSemaphoreInfo->handleType) {
                case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT:
                    pExternalSemaphoreProperties->exportFromImportedHandleTypes =
                        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
                    pExternalSemaphoreProperties->compatibleHandleTypes =
                        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
                    pExternalSemaphoreProperties->externalSemaphoreFeatures =
                        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
                        VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
                    return;
                case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT:
                    pExternalSemaphoreProperties->exportFromImportedHandleTypes =
                        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
                    pExternalSemaphoreProperties->compatibleHandleTypes =
                        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
                    pExternalSemaphoreProperties->externalSemaphoreFeatures =
                        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
                        VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
                    return;
                default:
                    break;
            }
        }

        pExternalSemaphoreProperties->exportFromImportedHandleTypes = 0;
        pExternalSemaphoreProperties->compatibleHandleTypes = 0;
        pExternalSemaphoreProperties->externalSemaphoreFeatures = 0;
    }

    VkResult on_vkCreateDescriptorUpdateTemplate(
        android::base::BumpPool* pool, VkDevice boxed_device,
        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        auto descriptorUpdateTemplateInfo = calcLinearizedDescriptorUpdateTemplateInfo(pCreateInfo);

        VkResult res =
            vk->vkCreateDescriptorUpdateTemplate(device, &descriptorUpdateTemplateInfo.createInfo,
                                                 pAllocator, pDescriptorUpdateTemplate);

        if (res == VK_SUCCESS) {
            registerDescriptorUpdateTemplate(*pDescriptorUpdateTemplate,
                                             descriptorUpdateTemplateInfo);
            *pDescriptorUpdateTemplate =
                new_boxed_non_dispatchable_VkDescriptorUpdateTemplate(*pDescriptorUpdateTemplate);
        }

        return res;
    }

    VkResult on_vkCreateDescriptorUpdateTemplateKHR(
        android::base::BumpPool* pool, VkDevice boxed_device,
        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        auto descriptorUpdateTemplateInfo = calcLinearizedDescriptorUpdateTemplateInfo(pCreateInfo);

        VkResult res = vk->vkCreateDescriptorUpdateTemplateKHR(
            device, &descriptorUpdateTemplateInfo.createInfo, pAllocator,
            pDescriptorUpdateTemplate);

        if (res == VK_SUCCESS) {
            registerDescriptorUpdateTemplate(*pDescriptorUpdateTemplate,
                                             descriptorUpdateTemplateInfo);
            *pDescriptorUpdateTemplate =
                new_boxed_non_dispatchable_VkDescriptorUpdateTemplate(*pDescriptorUpdateTemplate);
        }

        return res;
    }

    void on_vkDestroyDescriptorUpdateTemplate(android::base::BumpPool* pool, VkDevice boxed_device,
                                              VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                              const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        vk->vkDestroyDescriptorUpdateTemplate(device, descriptorUpdateTemplate, pAllocator);

        unregisterDescriptorUpdateTemplate(descriptorUpdateTemplate);
    }

    void on_vkDestroyDescriptorUpdateTemplateKHR(
        android::base::BumpPool* pool, VkDevice boxed_device,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        vk->vkDestroyDescriptorUpdateTemplateKHR(device, descriptorUpdateTemplate, pAllocator);

        unregisterDescriptorUpdateTemplate(descriptorUpdateTemplate);
    }

    void on_vkUpdateDescriptorSetWithTemplateSizedGOOGLE(
        android::base::BumpPool* pool, VkDevice boxed_device, VkDescriptorSet descriptorSet,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate, uint32_t imageInfoCount,
        uint32_t bufferInfoCount, uint32_t bufferViewCount, const uint32_t* pImageInfoEntryIndices,
        const uint32_t* pBufferInfoEntryIndices, const uint32_t* pBufferViewEntryIndices,
        const VkDescriptorImageInfo* pImageInfos, const VkDescriptorBufferInfo* pBufferInfos,
        const VkBufferView* pBufferViews) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* info = android::base::find(mDescriptorUpdateTemplateInfo, descriptorUpdateTemplate);
        if (!info) return;

        memcpy(info->data.data() + info->imageInfoStart, pImageInfos,
               imageInfoCount * sizeof(VkDescriptorImageInfo));
        memcpy(info->data.data() + info->bufferInfoStart, pBufferInfos,
               bufferInfoCount * sizeof(VkDescriptorBufferInfo));
        memcpy(info->data.data() + info->bufferViewStart, pBufferViews,
               bufferViewCount * sizeof(VkBufferView));

        vk->vkUpdateDescriptorSetWithTemplate(device, descriptorSet, descriptorUpdateTemplate,
                                              info->data.data());
    }

    void on_vkUpdateDescriptorSetWithTemplateSized2GOOGLE(
        android::base::BumpPool* pool, VkDevice boxed_device, VkDescriptorSet descriptorSet,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate, uint32_t imageInfoCount,
        uint32_t bufferInfoCount, uint32_t bufferViewCount, uint32_t inlineUniformBlockCount,
        const uint32_t* pImageInfoEntryIndices, const uint32_t* pBufferInfoEntryIndices,
        const uint32_t* pBufferViewEntryIndices, const VkDescriptorImageInfo* pImageInfos,
        const VkDescriptorBufferInfo* pBufferInfos, const VkBufferView* pBufferViews,
        const uint8_t* pInlineUniformBlockData) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto* info = android::base::find(mDescriptorUpdateTemplateInfo, descriptorUpdateTemplate);
        if (!info) return;

        memcpy(info->data.data() + info->imageInfoStart, pImageInfos,
               imageInfoCount * sizeof(VkDescriptorImageInfo));
        memcpy(info->data.data() + info->bufferInfoStart, pBufferInfos,
               bufferInfoCount * sizeof(VkDescriptorBufferInfo));
        memcpy(info->data.data() + info->bufferViewStart, pBufferViews,
               bufferViewCount * sizeof(VkBufferView));
        memcpy(info->data.data() + info->inlineUniformBlockStart, pInlineUniformBlockData,
               inlineUniformBlockCount);

        vk->vkUpdateDescriptorSetWithTemplate(device, descriptorSet, descriptorUpdateTemplate,
                                              info->data.data());
    }

    void hostSyncCommandBuffer(const char* tag, VkCommandBuffer boxed_commandBuffer,
                               uint32_t needHostSync, uint32_t sequenceNumber) {
        auto nextDeadline = []() {
            return android::base::getUnixTimeUs() + 10000;  // 10 ms
        };

        auto timeoutDeadline = android::base::getUnixTimeUs() + 5000000;  // 5 s

        OrderMaintenanceInfo* order = ordmaint_VkCommandBuffer(boxed_commandBuffer);
        if (!order) return;

        AutoLock lock(order->lock);

        if (needHostSync) {
            while (
                (sequenceNumber - __atomic_load_n(&order->sequenceNumber, __ATOMIC_ACQUIRE) != 1)) {
                auto waitUntilUs = nextDeadline();
                order->cv.timedWait(&order->lock, waitUntilUs);

                if (timeoutDeadline < android::base::getUnixTimeUs()) {
                    break;
                }
            }
        }

        __atomic_store_n(&order->sequenceNumber, sequenceNumber, __ATOMIC_RELEASE);
        order->cv.signal();
        releaseOrderMaintInfo(order);
    }

    void on_vkCommandBufferHostSyncGOOGLE(android::base::BumpPool* pool,
                                          VkCommandBuffer commandBuffer, uint32_t needHostSync,
                                          uint32_t sequenceNumber) {
        this->hostSyncCommandBuffer("hostSync", commandBuffer, needHostSync, sequenceNumber);
    }

    void hostSyncQueue(const char* tag, VkQueue boxed_queue, uint32_t needHostSync,
                       uint32_t sequenceNumber) {
        auto nextDeadline = []() {
            return android::base::getUnixTimeUs() + 10000;  // 10 ms
        };

        auto timeoutDeadline = android::base::getUnixTimeUs() + 5000000;  // 5 s

        OrderMaintenanceInfo* order = ordmaint_VkQueue(boxed_queue);
        if (!order) return;

        AutoLock lock(order->lock);

        if (needHostSync) {
            while (
                (sequenceNumber - __atomic_load_n(&order->sequenceNumber, __ATOMIC_ACQUIRE) != 1)) {
                auto waitUntilUs = nextDeadline();
                order->cv.timedWait(&order->lock, waitUntilUs);

                if (timeoutDeadline < android::base::getUnixTimeUs()) {
                    break;
                }
            }
        }

        __atomic_store_n(&order->sequenceNumber, sequenceNumber, __ATOMIC_RELEASE);
        order->cv.signal();
        releaseOrderMaintInfo(order);
    }

    void on_vkQueueHostSyncGOOGLE(android::base::BumpPool* pool, VkQueue queue,
                                  uint32_t needHostSync, uint32_t sequenceNumber) {
        this->hostSyncQueue("hostSyncQueue", queue, needHostSync, sequenceNumber);
    }

    VkResult on_vkCreateImageWithRequirementsGOOGLE(android::base::BumpPool* pool,
                                                    VkDevice boxed_device,
                                                    const VkImageCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkImage* pImage,
                                                    VkMemoryRequirements* pMemoryRequirements) {
        if (pMemoryRequirements) {
            memset(pMemoryRequirements, 0, sizeof(*pMemoryRequirements));
        }

        VkResult imageCreateRes =
            on_vkCreateImage(pool, boxed_device, pCreateInfo, pAllocator, pImage);

        if (imageCreateRes != VK_SUCCESS) {
            return imageCreateRes;
        }

        on_vkGetImageMemoryRequirements(pool, boxed_device, unbox_VkImage(*pImage),
                                        pMemoryRequirements);

        return imageCreateRes;
    }

    VkResult on_vkCreateBufferWithRequirementsGOOGLE(android::base::BumpPool* pool,
                                                     VkDevice boxed_device,
                                                     const VkBufferCreateInfo* pCreateInfo,
                                                     const VkAllocationCallbacks* pAllocator,
                                                     VkBuffer* pBuffer,
                                                     VkMemoryRequirements* pMemoryRequirements) {
        if (pMemoryRequirements) {
            memset(pMemoryRequirements, 0, sizeof(*pMemoryRequirements));
        }

        VkResult bufferCreateRes =
            on_vkCreateBuffer(pool, boxed_device, pCreateInfo, pAllocator, pBuffer);

        if (bufferCreateRes != VK_SUCCESS) {
            return bufferCreateRes;
        }

        on_vkGetBufferMemoryRequirements(pool, boxed_device, unbox_VkBuffer(*pBuffer),
                                         pMemoryRequirements);

        return bufferCreateRes;
    }

    VkResult on_vkBeginCommandBuffer(android::base::BumpPool* pool,
                                     VkCommandBuffer boxed_commandBuffer,
                                     const VkCommandBufferBeginInfo* pBeginInfo,
                                     const VkDecoderContext& context) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);
        VkResult result = vk->vkBeginCommandBuffer(commandBuffer, pBeginInfo);

        if (result != VK_SUCCESS) {
            return result;
        }

        m_emu->deviceLostHelper.onBeginCommandBuffer(commandBuffer, vk);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* commandBufferInfo = android::base::find(mCommandBufferInfo, commandBuffer);
        if (!commandBufferInfo) return VK_ERROR_UNKNOWN;
        commandBufferInfo->reset();

        if (context.processName) {
            commandBufferInfo->debugUtilsHelper.cmdBeginDebugLabel(commandBuffer, "Process %s",
                                                                   context.processName);
        }

        return VK_SUCCESS;
    }

    VkResult on_vkBeginCommandBufferAsyncGOOGLE(android::base::BumpPool* pool,
                                                VkCommandBuffer boxed_commandBuffer,
                                                const VkCommandBufferBeginInfo* pBeginInfo,
                                                const VkDecoderContext& context) {
        return this->on_vkBeginCommandBuffer(pool, boxed_commandBuffer, pBeginInfo, context);
    }

    VkResult on_vkEndCommandBuffer(android::base::BumpPool* pool,
                                   VkCommandBuffer boxed_commandBuffer,
                                   const VkDecoderContext& context) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);

        m_emu->deviceLostHelper.onEndCommandBuffer(commandBuffer, vk);

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* commandBufferInfo = android::base::find(mCommandBufferInfo, commandBuffer);
        if (!commandBufferInfo) return VK_ERROR_UNKNOWN;

        if (context.processName) {
            commandBufferInfo->debugUtilsHelper.cmdEndDebugLabel(commandBuffer);
        }

        return vk->vkEndCommandBuffer(commandBuffer);
    }

    void on_vkEndCommandBufferAsyncGOOGLE(android::base::BumpPool* pool,
                                          VkCommandBuffer boxed_commandBuffer,
                                          const VkDecoderContext& context) {
        on_vkEndCommandBuffer(pool, boxed_commandBuffer, context);
    }

    void on_vkResetCommandBufferAsyncGOOGLE(android::base::BumpPool* pool,
                                            VkCommandBuffer boxed_commandBuffer,
                                            VkCommandBufferResetFlags flags) {
        on_vkResetCommandBuffer(pool, boxed_commandBuffer, flags);
    }

    void on_vkCmdBindPipeline(android::base::BumpPool* pool, VkCommandBuffer boxed_commandBuffer,
                              VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);
        vk->vkCmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
        if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            auto* cmdBufferInfo = android::base::find(mCommandBufferInfo, commandBuffer);
            if (cmdBufferInfo) {
                cmdBufferInfo->computePipeline = pipeline;
            }
        }
    }

    void on_vkCmdBindDescriptorSets(android::base::BumpPool* pool,
                                    VkCommandBuffer boxed_commandBuffer,
                                    VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
                                    uint32_t firstSet, uint32_t descriptorSetCount,
                                    const VkDescriptorSet* pDescriptorSets,
                                    uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);
        vk->vkCmdBindDescriptorSets(commandBuffer, pipelineBindPoint, layout, firstSet,
                                    descriptorSetCount, pDescriptorSets, dynamicOffsetCount,
                                    pDynamicOffsets);
        if (descriptorSetCount) {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            auto* cmdBufferInfo = android::base::find(mCommandBufferInfo, commandBuffer);
            if (cmdBufferInfo) {
                cmdBufferInfo->descriptorLayout = layout;

                cmdBufferInfo->allDescriptorSets.insert(pDescriptorSets,
                                                        pDescriptorSets + descriptorSetCount);
                cmdBufferInfo->firstSet = firstSet;
                cmdBufferInfo->currentDescriptorSets.assign(pDescriptorSets,
                                                            pDescriptorSets + descriptorSetCount);
                cmdBufferInfo->dynamicOffsets.assign(pDynamicOffsets,
                                                     pDynamicOffsets + dynamicOffsetCount);
            }
        }
    }

    VkResult on_vkCreateRenderPass(android::base::BumpPool* pool, VkDevice boxed_device,
                                   const VkRenderPassCreateInfo* pCreateInfo,
                                   const VkAllocationCallbacks* pAllocator,
                                   VkRenderPass* pRenderPass) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);
        VkRenderPassCreateInfo createInfo;
        bool needReformat = false;
        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        if (deviceInfo->emulateTextureEtc2 || deviceInfo->emulateTextureAstc) {
            for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
                if (deviceInfo->needEmulatedDecompression(pCreateInfo->pAttachments[i].format)) {
                    needReformat = true;
                    break;
                }
            }
        }
        std::vector<VkAttachmentDescription> attachments;
        if (needReformat) {
            createInfo = *pCreateInfo;
            attachments.assign(pCreateInfo->pAttachments,
                               pCreateInfo->pAttachments + pCreateInfo->attachmentCount);
            createInfo.pAttachments = attachments.data();
            for (auto& attachment : attachments) {
                attachment.format = CompressedImageInfo::getOutputFormat(attachment.format);
            }
            pCreateInfo = &createInfo;
        }
        VkResult res = vk->vkCreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
        if (res != VK_SUCCESS) {
            return res;
        }

        auto& renderPassInfo = mRenderPassInfo[*pRenderPass];
        renderPassInfo.device = device;

        *pRenderPass = new_boxed_non_dispatchable_VkRenderPass(*pRenderPass);

        return res;
    }

    VkResult on_vkCreateRenderPass2(android::base::BumpPool* pool, VkDevice boxed_device,
                                    const VkRenderPassCreateInfo2* pCreateInfo,
                                    const VkAllocationCallbacks* pAllocator,
                                    VkRenderPass* pRenderPass) {
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);
        std::lock_guard<std::recursive_mutex> lock(mLock);

        VkResult res = vk->vkCreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
        if (res != VK_SUCCESS) {
            return res;
        }

        auto& renderPassInfo = mRenderPassInfo[*pRenderPass];
        renderPassInfo.device = device;

        *pRenderPass = new_boxed_non_dispatchable_VkRenderPass(*pRenderPass);

        return res;
    }

    void destroyRenderPassLocked(VkDevice device, VulkanDispatch* deviceDispatch,
                                 VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator) {
        deviceDispatch->vkDestroyRenderPass(device, renderPass, pAllocator);

        mRenderPassInfo.erase(renderPass);
    }

    void on_vkDestroyRenderPass(android::base::BumpPool* pool, VkDevice boxed_device,
                                VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        destroyRenderPassLocked(device, deviceDispatch, renderPass, pAllocator);
    }

    void registerRenderPassBeginInfo(VkCommandBuffer commandBuffer,
                                     const VkRenderPassBeginInfo* pRenderPassBegin) {
        CommandBufferInfo* cmdBufferInfo = android::base::find(mCommandBufferInfo, commandBuffer);
        FramebufferInfo* fbInfo =
            android::base::find(mFramebufferInfo, pRenderPassBegin->framebuffer);
        cmdBufferInfo->releasedColorBuffers.insert(fbInfo->attachedColorBuffers.begin(),
                                                   fbInfo->attachedColorBuffers.end());
    }

    void on_vkCmdBeginRenderPass(android::base::BumpPool* pool, VkCommandBuffer boxed_commandBuffer,
                                 const VkRenderPassBeginInfo* pRenderPassBegin,
                                 VkSubpassContents contents) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);
        registerRenderPassBeginInfo(commandBuffer, pRenderPassBegin);
        vk->vkCmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
    }

    void on_vkCmdBeginRenderPass2(android::base::BumpPool* pool,
                                  VkCommandBuffer boxed_commandBuffer,
                                  const VkRenderPassBeginInfo* pRenderPassBegin,
                                  const VkSubpassBeginInfo* pSubpassBeginInfo) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);
        registerRenderPassBeginInfo(commandBuffer, pRenderPassBegin);
        vk->vkCmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }

    void on_vkCmdBeginRenderPass2KHR(android::base::BumpPool* pool,
                                     VkCommandBuffer boxed_commandBuffer,
                                     const VkRenderPassBeginInfo* pRenderPassBegin,
                                     const VkSubpassBeginInfo* pSubpassBeginInfo) {
        on_vkCmdBeginRenderPass2(pool, boxed_commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }

    void on_vkCmdCopyQueryPoolResults(android::base::BumpPool* pool,
                                      VkCommandBuffer boxed_commandBuffer, VkQueryPool queryPool,
                                      uint32_t firstQuery, uint32_t queryCount, VkBuffer dstBuffer,
                                      VkDeviceSize dstOffset, VkDeviceSize stride,
                                      VkQueryResultFlags flags) {
        auto commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        auto vk = dispatch_VkCommandBuffer(boxed_commandBuffer);
        if (queryCount == 1 && stride == 0) {
            // Some drivers don't seem to handle stride==0 very well.
            // In fact, the spec does not say what should happen with stride==0.
            // So we just use the largest stride possible.
            stride = mBufferInfo[dstBuffer].size - dstOffset;
        }
        vk->vkCmdCopyQueryPoolResults(commandBuffer, queryPool, firstQuery, queryCount, dstBuffer,
                                      dstOffset, stride, flags);
    }

    VkResult on_vkCreateFramebuffer(android::base::BumpPool* pool, VkDevice boxed_device,
                                    const VkFramebufferCreateInfo* pCreateInfo,
                                    const VkAllocationCallbacks* pAllocator,
                                    VkFramebuffer* pFramebuffer) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        VkResult result =
            deviceDispatch->vkCreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
        if (result != VK_SUCCESS) {
            return result;
        }

        std::lock_guard<std::recursive_mutex> lock(mLock);

        auto& framebufferInfo = mFramebufferInfo[*pFramebuffer];
        framebufferInfo.device = device;

        if ((pCreateInfo->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) == 0) {
            // b/327522469
            // Track the Colorbuffers that would be written to.
            // It might be better to check for VK_QUEUE_FAMILY_EXTERNAL in pipeline barrier.
            // But the guest does not always add it to pipeline barrier.
            for (int i = 0; i < pCreateInfo->attachmentCount; i++) {
                auto* imageViewInfo = android::base::find(mImageViewInfo, pCreateInfo->pAttachments[i]);
                if (imageViewInfo->boundColorBuffer.has_value()) {
                    framebufferInfo.attachedColorBuffers.push_back(
                        imageViewInfo->boundColorBuffer.value());
                }
            }
        }

        *pFramebuffer = new_boxed_non_dispatchable_VkFramebuffer(*pFramebuffer);

        return result;
    }

    void destroyFramebufferLocked(VkDevice device, VulkanDispatch* deviceDispatch,
                                  VkFramebuffer framebuffer,
                                  const VkAllocationCallbacks* pAllocator) {
        deviceDispatch->vkDestroyFramebuffer(device, framebuffer, pAllocator);

        mFramebufferInfo.erase(framebuffer);
    }

    void on_vkDestroyFramebuffer(android::base::BumpPool* pool, VkDevice boxed_device,
                                 VkFramebuffer framebuffer,
                                 const VkAllocationCallbacks* pAllocator) {
        auto device = unbox_VkDevice(boxed_device);
        auto deviceDispatch = dispatch_VkDevice(boxed_device);

        std::lock_guard<std::recursive_mutex> lock(mLock);
        destroyFramebufferLocked(device, deviceDispatch, framebuffer, pAllocator);
    }

    VkResult on_vkQueueBindSparse(android::base::BumpPool* pool, VkQueue boxed_queue,
                                  uint32_t bindInfoCount, const VkBindSparseInfo* pBindInfo,
                                  VkFence fence) {
        // If pBindInfo contains VkTimelineSemaphoreSubmitInfo, then it's
        // possible the host driver isn't equipped to deal with them yet.  To
        // work around this, send empty vkQueueSubmits before and after the
        // call to vkQueueBindSparse that contain the right values for
        // wait/signal semaphores and contains the user's
        // VkTimelineSemaphoreSubmitInfo structure, following the *submission
        // order* implied by the indices of pBindInfo.

        // TODO: Detect if we are running on a driver that supports timeline
        // semaphore signal/wait operations in vkQueueBindSparse
        const bool needTimelineSubmitInfoWorkaround = true;
        (void)needTimelineSubmitInfoWorkaround;

        bool hasTimelineSemaphoreSubmitInfo = false;

        for (uint32_t i = 0; i < bindInfoCount; ++i) {
            const VkTimelineSemaphoreSubmitInfoKHR* tsSi =
                vk_find_struct<VkTimelineSemaphoreSubmitInfoKHR>(pBindInfo + i);
            if (tsSi) {
                hasTimelineSemaphoreSubmitInfo = true;
            }
        }

        auto queue = unbox_VkQueue(boxed_queue);
        auto vk = dispatch_VkQueue(boxed_queue);

        if (!hasTimelineSemaphoreSubmitInfo) {
            (void)pool;
            return vk->vkQueueBindSparse(queue, bindInfoCount, pBindInfo, fence);
        } else {
            std::vector<VkPipelineStageFlags> waitDstStageMasks;
            VkTimelineSemaphoreSubmitInfoKHR currTsSi = {
                VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, 0, 0, nullptr, 0, nullptr,
            };

            VkSubmitInfo currSi = {
                VK_STRUCTURE_TYPE_SUBMIT_INFO,
                &currTsSi,
                0,
                nullptr,
                nullptr,
                0,
                nullptr,  // No commands
                0,
                nullptr,
            };

            VkBindSparseInfo currBi;

            VkResult res;

            for (uint32_t i = 0; i < bindInfoCount; ++i) {
                const VkTimelineSemaphoreSubmitInfoKHR* tsSi =
                    vk_find_struct<VkTimelineSemaphoreSubmitInfoKHR>(pBindInfo + i);
                if (!tsSi) {
                    res = vk->vkQueueBindSparse(queue, 1, pBindInfo + i, fence);
                    if (VK_SUCCESS != res) return res;
                    continue;
                }

                currTsSi.waitSemaphoreValueCount = tsSi->waitSemaphoreValueCount;
                currTsSi.pWaitSemaphoreValues = tsSi->pWaitSemaphoreValues;
                currTsSi.signalSemaphoreValueCount = 0;
                currTsSi.pSignalSemaphoreValues = nullptr;

                currSi.waitSemaphoreCount = pBindInfo[i].waitSemaphoreCount;
                currSi.pWaitSemaphores = pBindInfo[i].pWaitSemaphores;
                waitDstStageMasks.resize(pBindInfo[i].waitSemaphoreCount,
                                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
                currSi.pWaitDstStageMask = waitDstStageMasks.data();

                currSi.signalSemaphoreCount = 0;
                currSi.pSignalSemaphores = nullptr;

                res = vk->vkQueueSubmit(queue, 1, &currSi, nullptr);
                if (VK_SUCCESS != res) return res;

                currBi = pBindInfo[i];

                vk_struct_chain_remove(tsSi, &currBi);

                currBi.waitSemaphoreCount = 0;
                currBi.pWaitSemaphores = nullptr;
                currBi.signalSemaphoreCount = 0;
                currBi.pSignalSemaphores = nullptr;

                res = vk->vkQueueBindSparse(queue, 1, &currBi, nullptr);
                if (VK_SUCCESS != res) return res;

                currTsSi.waitSemaphoreValueCount = 0;
                currTsSi.pWaitSemaphoreValues = nullptr;
                currTsSi.signalSemaphoreValueCount = tsSi->signalSemaphoreValueCount;
                currTsSi.pSignalSemaphoreValues = tsSi->pSignalSemaphoreValues;

                currSi.waitSemaphoreCount = 0;
                currSi.pWaitSemaphores = nullptr;
                currSi.signalSemaphoreCount = pBindInfo[i].signalSemaphoreCount;
                currSi.pSignalSemaphores = pBindInfo[i].pSignalSemaphores;

                res =
                    vk->vkQueueSubmit(queue, 1, &currSi, i == bindInfoCount - 1 ? fence : nullptr);
                if (VK_SUCCESS != res) return res;
            }

            return VK_SUCCESS;
        }
    }

    void on_vkGetLinearImageLayoutGOOGLE(android::base::BumpPool* pool, VkDevice boxed_device,
                                         VkFormat format, VkDeviceSize* pOffset,
                                         VkDeviceSize* pRowPitchAlignment) {
        if (mPerFormatLinearImageProperties.find(format) == mPerFormatLinearImageProperties.end()) {
            VkDeviceSize offset = 0u;
            VkDeviceSize rowPitchAlignment = UINT_MAX;

            for (uint32_t width = 64; width <= 256; width++) {
                LinearImageCreateInfo linearImageCreateInfo = {
                    .extent =
                        {
                            .width = width,
                            .height = 64,
                            .depth = 1,
                        },
                    .format = format,
                    .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                };

                VkDeviceSize currOffset = 0u;
                VkDeviceSize currRowPitchAlignment = UINT_MAX;

                VkImageCreateInfo defaultVkImageCreateInfo = linearImageCreateInfo.toDefaultVk();
                on_vkGetLinearImageLayout2GOOGLE(pool, boxed_device, &defaultVkImageCreateInfo,
                                                 &currOffset, &currRowPitchAlignment);

                offset = currOffset;
                rowPitchAlignment = std::min(currRowPitchAlignment, rowPitchAlignment);
            }
            mPerFormatLinearImageProperties[format] = LinearImageProperties{
                .offset = offset,
                .rowPitchAlignment = rowPitchAlignment,
            };
        }

        if (pOffset) {
            *pOffset = mPerFormatLinearImageProperties[format].offset;
        }
        if (pRowPitchAlignment) {
            *pRowPitchAlignment = mPerFormatLinearImageProperties[format].rowPitchAlignment;
        }
    }

    void on_vkGetLinearImageLayout2GOOGLE(android::base::BumpPool* pool, VkDevice boxed_device,
                                          const VkImageCreateInfo* pCreateInfo,
                                          VkDeviceSize* pOffset, VkDeviceSize* pRowPitchAlignment) {
        LinearImageCreateInfo linearImageCreateInfo = {
            .extent = pCreateInfo->extent,
            .format = pCreateInfo->format,
            .usage = pCreateInfo->usage,
        };
        if (mLinearImageProperties.find(linearImageCreateInfo) == mLinearImageProperties.end()) {
            auto device = unbox_VkDevice(boxed_device);
            auto vk = dispatch_VkDevice(boxed_device);

            VkImageSubresource subresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .arrayLayer = 0,
            };

            VkImage image;
            VkSubresourceLayout subresourceLayout;

            VkImageCreateInfo defaultVkImageCreateInfo = linearImageCreateInfo.toDefaultVk();
            VkResult result = vk->vkCreateImage(device, &defaultVkImageCreateInfo, nullptr, &image);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "vkCreateImage failed. size: (%u x %u) result: %d\n",
                        linearImageCreateInfo.extent.width, linearImageCreateInfo.extent.height,
                        result);
                return;
            }
            vk->vkGetImageSubresourceLayout(device, image, &subresource, &subresourceLayout);
            vk->vkDestroyImage(device, image, nullptr);

            VkDeviceSize offset = subresourceLayout.offset;
            uint64_t rowPitch = subresourceLayout.rowPitch;
            VkDeviceSize rowPitchAlignment = rowPitch & (~rowPitch + 1);

            mLinearImageProperties[linearImageCreateInfo] = {
                .offset = offset,
                .rowPitchAlignment = rowPitchAlignment,
            };
        }

        if (pOffset != nullptr) {
            *pOffset = mLinearImageProperties[linearImageCreateInfo].offset;
        }
        if (pRowPitchAlignment != nullptr) {
            *pRowPitchAlignment = mLinearImageProperties[linearImageCreateInfo].rowPitchAlignment;
        }
    }

#include "VkSubDecoder.cpp"

    void on_vkQueueFlushCommandsGOOGLE(android::base::BumpPool* pool, VkQueue queue,
                                       VkCommandBuffer boxed_commandBuffer, VkDeviceSize dataSize,
                                       const void* pData, const VkDecoderContext& context) {
        (void)queue;

        VkCommandBuffer commandBuffer = unbox_VkCommandBuffer(boxed_commandBuffer);
        VulkanDispatch* vk = dispatch_VkCommandBuffer(boxed_commandBuffer);
        VulkanMemReadingStream* readStream = readstream_VkCommandBuffer(boxed_commandBuffer);
        subDecode(readStream, vk, boxed_commandBuffer, commandBuffer, dataSize, pData, context);
    }

    void on_vkQueueFlushCommandsFromAuxMemoryGOOGLE(android::base::BumpPool* pool, VkQueue queue,
                                                    VkCommandBuffer commandBuffer,
                                                    VkDeviceMemory deviceMemory,
                                                    VkDeviceSize dataOffset, VkDeviceSize dataSize,
                                                    const VkDecoderContext& context) {
        // TODO : implement
    }
    VkDescriptorSet getOrAllocateDescriptorSetFromPoolAndId(VulkanDispatch* vk, VkDevice device,
                                                            VkDescriptorPool pool,
                                                            VkDescriptorSetLayout setLayout,
                                                            uint64_t poolId, uint32_t pendingAlloc,
                                                            bool* didAlloc) {
        auto* poolInfo = android::base::find(mDescriptorPoolInfo, pool);
        if (!poolInfo) {
            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                << "descriptor pool " << pool << " not found ";
        }

        DispatchableHandleInfo<uint64_t>* setHandleInfo = sBoxedHandleManager.get(poolId);

        if (setHandleInfo->underlying) {
            if (pendingAlloc) {
                VkDescriptorSet allocedSet;
                vk->vkFreeDescriptorSets(device, pool, 1,
                                         (VkDescriptorSet*)(&setHandleInfo->underlying));
                VkDescriptorSetAllocateInfo dsAi = {
                    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, pool, 1, &setLayout,
                };
                vk->vkAllocateDescriptorSets(device, &dsAi, &allocedSet);
                setHandleInfo->underlying = (uint64_t)allocedSet;
                initDescriptorSetInfoLocked(pool, setLayout, poolId, allocedSet);
                *didAlloc = true;
                return allocedSet;
            } else {
                *didAlloc = false;
                return (VkDescriptorSet)(setHandleInfo->underlying);
            }
        } else {
            if (pendingAlloc) {
                VkDescriptorSet allocedSet;
                VkDescriptorSetAllocateInfo dsAi = {
                    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, pool, 1, &setLayout,
                };
                vk->vkAllocateDescriptorSets(device, &dsAi, &allocedSet);
                setHandleInfo->underlying = (uint64_t)allocedSet;
                initDescriptorSetInfoLocked(pool, setLayout, poolId, allocedSet);
                *didAlloc = true;
                return allocedSet;
            } else {
                GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                    << "descriptor pool " << pool << " wanted to get set with id 0x" << std::hex
                    << poolId;
                return nullptr;
            }
        }
    }

    void on_vkQueueCommitDescriptorSetUpdatesGOOGLE(
        android::base::BumpPool* pool, VkQueue boxed_queue, uint32_t descriptorPoolCount,
        const VkDescriptorPool* pDescriptorPools, uint32_t descriptorSetCount,
        const VkDescriptorSetLayout* pDescriptorSetLayouts, const uint64_t* pDescriptorSetPoolIds,
        const uint32_t* pDescriptorSetWhichPool, const uint32_t* pDescriptorSetPendingAllocation,
        const uint32_t* pDescriptorWriteStartingIndices, uint32_t pendingDescriptorWriteCount,
        const VkWriteDescriptorSet* pPendingDescriptorWrites) {
        std::lock_guard<std::recursive_mutex> lock(mLock);

        VkDevice device;

        auto queue = unbox_VkQueue(boxed_queue);
        auto vk = dispatch_VkQueue(boxed_queue);

        auto* queueInfo = android::base::find(mQueueInfo, queue);
        if (queueInfo) {
            device = queueInfo->device;
        } else {
            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                << "queue " << queue << "(boxed: " << boxed_queue << ") with no device registered";
        }
        on_vkQueueCommitDescriptorSetUpdatesGOOGLE(
            pool, vk, device, descriptorPoolCount, pDescriptorPools, descriptorSetCount,
            pDescriptorSetLayouts, pDescriptorSetPoolIds, pDescriptorSetWhichPool,
            pDescriptorSetPendingAllocation, pDescriptorWriteStartingIndices,
            pendingDescriptorWriteCount, pPendingDescriptorWrites);
    }

    void on_vkQueueCommitDescriptorSetUpdatesGOOGLE(
        android::base::BumpPool* pool, VulkanDispatch* vk, VkDevice device,
        uint32_t descriptorPoolCount, const VkDescriptorPool* pDescriptorPools,
        uint32_t descriptorSetCount, const VkDescriptorSetLayout* pDescriptorSetLayouts,
        const uint64_t* pDescriptorSetPoolIds, const uint32_t* pDescriptorSetWhichPool,
        const uint32_t* pDescriptorSetPendingAllocation,
        const uint32_t* pDescriptorWriteStartingIndices, uint32_t pendingDescriptorWriteCount,
        const VkWriteDescriptorSet* pPendingDescriptorWrites) {
        std::vector<VkDescriptorSet> setsToUpdate(descriptorSetCount, nullptr);

        bool didAlloc = false;

        for (uint32_t i = 0; i < descriptorSetCount; ++i) {
            uint64_t poolId = pDescriptorSetPoolIds[i];
            uint32_t whichPool = pDescriptorSetWhichPool[i];
            uint32_t pendingAlloc = pDescriptorSetPendingAllocation[i];
            bool didAllocThisTime;
            setsToUpdate[i] = getOrAllocateDescriptorSetFromPoolAndId(
                vk, device, pDescriptorPools[whichPool], pDescriptorSetLayouts[i], poolId,
                pendingAlloc, &didAllocThisTime);

            if (didAllocThisTime) didAlloc = true;
        }

        if (didAlloc) {
            std::vector<VkWriteDescriptorSet> writeDescriptorSetsForHostDriver(
                pendingDescriptorWriteCount);
            memcpy(writeDescriptorSetsForHostDriver.data(), pPendingDescriptorWrites,
                   pendingDescriptorWriteCount * sizeof(VkWriteDescriptorSet));

            for (uint32_t i = 0; i < descriptorSetCount; ++i) {
                uint32_t writeStartIndex = pDescriptorWriteStartingIndices[i];
                uint32_t writeEndIndex;
                if (i == descriptorSetCount - 1) {
                    writeEndIndex = pendingDescriptorWriteCount;
                } else {
                    writeEndIndex = pDescriptorWriteStartingIndices[i + 1];
                }
                for (uint32_t j = writeStartIndex; j < writeEndIndex; ++j) {
                    writeDescriptorSetsForHostDriver[j].dstSet = setsToUpdate[i];
                }
            }
            this->on_vkUpdateDescriptorSetsImpl(
                pool, vk, device, (uint32_t)writeDescriptorSetsForHostDriver.size(),
                writeDescriptorSetsForHostDriver.data(), 0, nullptr);
        } else {
            this->on_vkUpdateDescriptorSetsImpl(pool, vk, device, pendingDescriptorWriteCount,
                                                pPendingDescriptorWrites, 0, nullptr);
        }
    }

    void on_vkCollectDescriptorPoolIdsGOOGLE(android::base::BumpPool* pool, VkDevice device,
                                             VkDescriptorPool descriptorPool,
                                             uint32_t* pPoolIdCount, uint64_t* pPoolIds) {
        std::lock_guard<std::recursive_mutex> lock(mLock);
        auto& info = mDescriptorPoolInfo[descriptorPool];
        *pPoolIdCount = (uint32_t)info.poolIds.size();

        if (pPoolIds) {
            for (uint32_t i = 0; i < info.poolIds.size(); ++i) {
                pPoolIds[i] = info.poolIds[i];
            }
        }
    }

    VkResult on_vkCreateSamplerYcbcrConversion(
        android::base::BumpPool*, VkDevice boxed_device,
        const VkSamplerYcbcrConversionCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator, VkSamplerYcbcrConversion* pYcbcrConversion) {
        if (m_emu->enableYcbcrEmulation && !m_emu->deviceInfo.supportsSamplerYcbcrConversion) {
            *pYcbcrConversion = new_boxed_non_dispatchable_VkSamplerYcbcrConversion(
                (VkSamplerYcbcrConversion)((uintptr_t)0xffff0000ull));
            return VK_SUCCESS;
        }
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);
        VkResult res =
            vk->vkCreateSamplerYcbcrConversion(device, pCreateInfo, pAllocator, pYcbcrConversion);
        if (res != VK_SUCCESS) {
            return res;
        }
        *pYcbcrConversion = new_boxed_non_dispatchable_VkSamplerYcbcrConversion(*pYcbcrConversion);
        return VK_SUCCESS;
    }

    void on_vkDestroySamplerYcbcrConversion(android::base::BumpPool* pool, VkDevice boxed_device,
                                            VkSamplerYcbcrConversion ycbcrConversion,
                                            const VkAllocationCallbacks* pAllocator) {
        if (m_emu->enableYcbcrEmulation && !m_emu->deviceInfo.supportsSamplerYcbcrConversion) {
            return;
        }
        auto device = unbox_VkDevice(boxed_device);
        auto vk = dispatch_VkDevice(boxed_device);
        vk->vkDestroySamplerYcbcrConversion(device, ycbcrConversion, pAllocator);
        return;
    }

    VkResult on_vkEnumeratePhysicalDeviceGroups(
        android::base::BumpPool* pool, VkInstance boxed_instance,
        uint32_t* pPhysicalDeviceGroupCount,
        VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties) {
        auto instance = unbox_VkInstance(boxed_instance);
        auto vk = dispatch_VkInstance(boxed_instance);

        std::vector<VkPhysicalDevice> physicalDevices;
        auto res = GetPhysicalDevices(instance, vk, physicalDevices);
        if (res != VK_SUCCESS) {
            return res;
        }

        {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            FilterPhysicalDevicesLocked(instance, vk, physicalDevices);
        }

        const uint32_t requestedCount = pPhysicalDeviceGroupCount ? *pPhysicalDeviceGroupCount : 0;
        const uint32_t availableCount = static_cast<uint32_t>(physicalDevices.size());

        if (pPhysicalDeviceGroupCount) {
            *pPhysicalDeviceGroupCount = availableCount;
        }
        if (pPhysicalDeviceGroupCount && pPhysicalDeviceGroupProperties) {
            for (uint32_t i = 0; i < std::min(requestedCount, availableCount); ++i) {
                pPhysicalDeviceGroupProperties[i] = VkPhysicalDeviceGroupProperties{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES,
                    .pNext = nullptr,
                    .physicalDeviceCount = 1,
                    .physicalDevices =
                        {
                            unboxed_to_boxed_VkPhysicalDevice(physicalDevices[i]),
                        },
                    .subsetAllocation = VK_FALSE,
                };
            }
            if (requestedCount < availableCount) {
                return VK_INCOMPLETE;
            }
        }

        return VK_SUCCESS;
    }

    void on_DeviceLost() {
        {
            std::lock_guard<std::recursive_mutex> lock(mLock);

            std::vector<DeviceLostHelper::DeviceWithQueues> devicesToQueues;
            for (const auto& [device, deviceInfo] : mDeviceInfo) {
                auto& deviceToQueues = devicesToQueues.emplace_back();
                deviceToQueues.device = device;
                deviceToQueues.deviceDispatch = dispatch_VkDevice(deviceInfo.boxed);
                for (const auto& [queueIndex, queues] : deviceInfo.queues) {
                    deviceToQueues.queues.insert(deviceToQueues.queues.end(), queues.begin(),
                                                 queues.end());
                }
            }
            m_emu->deviceLostHelper.onDeviceLost(devicesToQueues);
        }

        GFXSTREAM_ABORT(FatalError(VK_ERROR_DEVICE_LOST));
    }

    void on_CheckOutOfMemory(VkResult result, uint32_t opCode, const VkDecoderContext& context,
                             std::optional<uint64_t> allocationSize = std::nullopt) {
        if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
            result == VK_ERROR_OUT_OF_POOL_MEMORY) {
            context.metricsLogger->logMetricEvent(
                MetricEventVulkanOutOfMemory{.vkResultCode = result,
                                             .opCode = std::make_optional(opCode),
                                             .allocationSize = allocationSize});
        }
    }

    VkResult waitForFence(VkFence boxed_fence, uint64_t timeout) {
        VkFence fence = unbox_VkFence(boxed_fence);
        VkDevice device;
        VulkanDispatch* vk;
        StaticLock* fenceLock;
        ConditionVariable* cv;
        {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            if (fence == VK_NULL_HANDLE || mFenceInfo.find(fence) == mFenceInfo.end()) {
                // No fence, could be a semaphore.
                // TODO: Async wait for semaphores
                return VK_SUCCESS;
            }

            // Vulkan specs require fences of vkQueueSubmit to be *externally
            // synchronized*, i.e. we cannot submit a queue while waiting for the
            // fence in another thread. For threads that call this function, they
            // have to wait until a vkQueueSubmit() using this fence is called
            // before calling vkWaitForFences(). So we use a conditional variable
            // and mutex for thread synchronization.
            //
            // See:
            // https://www.khronos.org/registry/vulkan/specs/1.2/html/vkspec.html#fundamentals-threadingbehavior
            // https://github.com/KhronosGroup/Vulkan-LoaderAndValidationLayers/issues/519

            device = mFenceInfo[fence].device;
            vk = mFenceInfo[fence].vk;
            fenceLock = &mFenceInfo[fence].lock;
            cv = &mFenceInfo[fence].cv;
        }

        fenceLock->lock();
        cv->wait(fenceLock, [this, fence] {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            if (mFenceInfo[fence].state == FenceInfo::State::kWaitable) {
                mFenceInfo[fence].state = FenceInfo::State::kWaiting;
                return true;
            }
            return false;
        });
        fenceLock->unlock();

        {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            if (mFenceInfo.find(fence) == mFenceInfo.end()) {
                GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                    << "Fence was destroyed before vkWaitForFences call.";
            }
        }

        return vk->vkWaitForFences(device, /* fenceCount */ 1u, &fence,
                                   /* waitAll */ false, timeout);
    }

    VkResult getFenceStatus(VkFence boxed_fence) {
        VkFence fence = unbox_VkFence(boxed_fence);
        VkDevice device;
        VulkanDispatch* vk;
        {
            std::lock_guard<std::recursive_mutex> lock(mLock);
            if (fence == VK_NULL_HANDLE || mFenceInfo.find(fence) == mFenceInfo.end()) {
                // No fence, could be a semaphore.
                // TODO: Async get status for semaphores
                return VK_SUCCESS;
            }

            device = mFenceInfo[fence].device;
            vk = mFenceInfo[fence].vk;
        }

        return vk->vkGetFenceStatus(device, fence);
    }

    AsyncResult registerQsriCallback(VkImage boxed_image, VkQsriTimeline::Callback callback) {
        VkImage image;
        std::shared_ptr<AndroidNativeBufferInfo> anbInfo;
        {
            std::lock_guard<std::recursive_mutex> lock(mLock);

            image = unbox_VkImage(boxed_image);

            if (mLogging) {
                fprintf(stderr, "%s: for boxed image 0x%llx image %p\n", __func__,
                        (unsigned long long)boxed_image, image);
            }

            if (image == VK_NULL_HANDLE || mImageInfo.find(image) == mImageInfo.end()) {
                // No image
                return AsyncResult::FAIL_AND_CALLBACK_NOT_SCHEDULED;
            }

            anbInfo = mImageInfo[image].anbInfo;  // shared ptr, take ref
        }

        if (!anbInfo) {
            fprintf(stderr, "%s: warning: image %p doesn't ahve anb info\n", __func__, image);
            return AsyncResult::FAIL_AND_CALLBACK_NOT_SCHEDULED;
        }
        if (!anbInfo->vk) {
            fprintf(stderr, "%s:%p warning: image %p anb info not initialized\n", __func__,
                    anbInfo.get(), image);
            return AsyncResult::FAIL_AND_CALLBACK_NOT_SCHEDULED;
        }
        // Could be null or mismatched image, check later
        if (image != anbInfo->image) {
            fprintf(stderr, "%s:%p warning: image %p anb info has wrong image: %p\n", __func__,
                    anbInfo.get(), image, anbInfo->image);
            return AsyncResult::FAIL_AND_CALLBACK_NOT_SCHEDULED;
        }

        anbInfo->qsriTimeline->registerCallbackForNextPresentAndPoll(std::move(callback));

        if (mLogging) {
            fprintf(stderr, "%s:%p Done registering\n", __func__, anbInfo.get());
        }
        return AsyncResult::OK_AND_CALLBACK_SCHEDULED;
    }

#define GUEST_EXTERNAL_MEMORY_HANDLE_TYPES                                \
    (VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID | \
     VK_EXTERNAL_MEMORY_HANDLE_TYPE_ZIRCON_VMO_BIT_FUCHSIA)

    // Transforms
    // If adding a new transform here, please check if it needs to be used in VkDecoderTestDispatch

    void transformImpl_VkExternalMemoryProperties_tohost(const VkExternalMemoryProperties* props,
                                                         uint32_t count) {
        VkExternalMemoryProperties* mut = (VkExternalMemoryProperties*)props;
        for (uint32_t i = 0; i < count; ++i) {
            mut[i] = transformExternalMemoryProperties_tohost(mut[i]);
        }
    }
    void transformImpl_VkExternalMemoryProperties_fromhost(const VkExternalMemoryProperties* props,
                                                           uint32_t count) {
        VkExternalMemoryProperties* mut = (VkExternalMemoryProperties*)props;
        for (uint32_t i = 0; i < count; ++i) {
            mut[i] = transformExternalMemoryProperties_fromhost(mut[i],
                                                                GUEST_EXTERNAL_MEMORY_HANDLE_TYPES);
        }
    }

    void transformImpl_VkImageCreateInfo_tohost(const VkImageCreateInfo* pImageCreateInfos,
                                                uint32_t count) {
        for (uint32_t i = 0; i < count; i++) {
            VkImageCreateInfo& imageCreateInfo =
                const_cast<VkImageCreateInfo&>(pImageCreateInfos[i]);
            const VkExternalMemoryImageCreateInfo* pExternalMemoryImageCi =
                vk_find_struct<VkExternalMemoryImageCreateInfo>(&imageCreateInfo);
            bool importAndroidHardwareBuffer =
                pExternalMemoryImageCi &&
                (pExternalMemoryImageCi->handleTypes &
                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
            const VkNativeBufferANDROID* pNativeBufferANDROID =
                vk_find_struct<VkNativeBufferANDROID>(&imageCreateInfo);

            // If the VkImage is going to bind to a ColorBuffer, we have to make sure the VkImage
            // that backs the ColorBuffer is created with identical parameters. From the spec: If
            // two aliases are both images that were created with identical creation parameters,
            // both were created with the VK_IMAGE_CREATE_ALIAS_BIT flag set, and both are bound
            // identically to memory except for VkBindImageMemoryDeviceGroupInfo::pDeviceIndices and
            // VkBindImageMemoryDeviceGroupInfo::pSplitInstanceBindRegions, then they interpret the
            // contents of the memory in consistent ways, and data written to one alias can be read
            // by the other alias. ... Aliases created by binding the same memory to resources in
            // multiple Vulkan instances or external APIs using external memory handle export and
            // import mechanisms interpret the contents of the memory in consistent ways, and data
            // written to one alias can be read by the other alias. Otherwise, the aliases interpret
            // the contents of the memory differently, ...
            std::unique_ptr<VkImageCreateInfo> colorBufferVkImageCi = nullptr;
            std::string importSource;
            VkFormat resolvedFormat = VK_FORMAT_UNDEFINED;
            // Use UNORM formats for SRGB format requests.
            switch (imageCreateInfo.format) {
                case VK_FORMAT_R8G8B8A8_SRGB:
                    resolvedFormat = VK_FORMAT_R8G8B8A8_UNORM;
                    break;
                case VK_FORMAT_R8G8B8_SRGB:
                    resolvedFormat = VK_FORMAT_R8G8B8_UNORM;
                    break;
                case VK_FORMAT_B8G8R8A8_SRGB:
                    resolvedFormat = VK_FORMAT_B8G8R8A8_UNORM;
                    break;
                case VK_FORMAT_R8_SRGB:
                    resolvedFormat = VK_FORMAT_R8_UNORM;
                    break;
                default:
                    resolvedFormat = imageCreateInfo.format;
            }
            if (importAndroidHardwareBuffer) {
                // For AHardwareBufferImage binding, we can't know which ColorBuffer this
                // to-be-created VkImage will bind to, so we try our best to infer the creation
                // parameters.
                colorBufferVkImageCi = generateColorBufferVkImageCreateInfo(
                    resolvedFormat, imageCreateInfo.extent.width, imageCreateInfo.extent.height,
                    imageCreateInfo.tiling);
                importSource = "AHardwareBuffer";
            } else if (pNativeBufferANDROID) {
                // For native buffer binding, we can query the creation parameters from handle.
                uint32_t cbHandle = *static_cast<const uint32_t*>(pNativeBufferANDROID->handle);
                auto colorBufferInfo = getColorBufferInfo(cbHandle);
                if (colorBufferInfo.handle == cbHandle) {
                    colorBufferVkImageCi =
                        std::make_unique<VkImageCreateInfo>(colorBufferInfo.imageCreateInfoShallow);
                } else {
                    ERR("Unknown ColorBuffer handle: %" PRIu32 ".", cbHandle);
                }
                importSource = "NativeBufferANDROID";
            }
            if (!colorBufferVkImageCi) {
                continue;
            }
            imageCreateInfo.format = resolvedFormat;
            if (imageCreateInfo.flags & (~colorBufferVkImageCi->flags)) {
                ERR("The VkImageCreateInfo to import %s contains unsupported VkImageCreateFlags. "
                    "All supported VkImageCreateFlags are %s, the input VkImageCreateInfo requires "
                    "support for %s.",
                    importSource.c_str()?:"",
                    string_VkImageCreateFlags(colorBufferVkImageCi->flags).c_str()?:"",
                    string_VkImageCreateFlags(imageCreateInfo.flags).c_str()?:"");
            }
            imageCreateInfo.flags |= colorBufferVkImageCi->flags;
            if (imageCreateInfo.imageType != colorBufferVkImageCi->imageType) {
                ERR("The VkImageCreateInfo to import %s has an unexpected VkImageType: %s, %s "
                    "expected.",
                    importSource.c_str()?:"", string_VkImageType(imageCreateInfo.imageType),
                    string_VkImageType(colorBufferVkImageCi->imageType));
            }
            if (imageCreateInfo.extent.depth != colorBufferVkImageCi->extent.depth) {
                ERR("The VkImageCreateInfo to import %s has an unexpected VkExtent::depth: %" PRIu32
                    ", %" PRIu32 " expected.",
                    importSource.c_str()?:"", imageCreateInfo.extent.depth,
                    colorBufferVkImageCi->extent.depth);
            }
            if (imageCreateInfo.mipLevels != colorBufferVkImageCi->mipLevels) {
                ERR("The VkImageCreateInfo to import %s has an unexpected mipLevels: %" PRIu32
                    ", %" PRIu32 " expected.",
                    importSource.c_str()?:"", imageCreateInfo.mipLevels,
                    colorBufferVkImageCi->mipLevels);
            }
            if (imageCreateInfo.arrayLayers != colorBufferVkImageCi->arrayLayers) {
                ERR("The VkImageCreateInfo to import %s has an unexpected arrayLayers: %" PRIu32
                    ", %" PRIu32 " expected.",
                    importSource.c_str()?:"", imageCreateInfo.arrayLayers,
                    colorBufferVkImageCi->arrayLayers);
            }
            if (imageCreateInfo.samples != colorBufferVkImageCi->samples) {
                ERR("The VkImageCreateInfo to import %s has an unexpected VkSampleCountFlagBits: "
                    "%s, %s expected.",
                    importSource.c_str()?:"", string_VkSampleCountFlagBits(imageCreateInfo.samples),
                    string_VkSampleCountFlagBits(colorBufferVkImageCi->samples));
            }
            if (imageCreateInfo.usage & (~colorBufferVkImageCi->usage)) {
                ERR("The VkImageCreateInfo to import %s contains unsupported VkImageUsageFlags. "
                    "All supported VkImageUsageFlags are %s, the input VkImageCreateInfo requires "
                    "support for %s.",
                    importSource.c_str()?:"",
                    string_VkImageUsageFlags(colorBufferVkImageCi->usage).c_str()?:"",
                    string_VkImageUsageFlags(imageCreateInfo.usage).c_str()?:"");
            }
            imageCreateInfo.usage |= colorBufferVkImageCi->usage;
            // For the AndroidHardwareBuffer binding case VkImageCreateInfo::sharingMode isn't
            // filled in generateColorBufferVkImageCreateInfo, and
            // VkImageCreateInfo::{format,extent::{width, height}, tiling} are guaranteed to match.
            if (importAndroidHardwareBuffer) {
                continue;
            }
            if (resolvedFormat != colorBufferVkImageCi->format) {
                ERR("The VkImageCreateInfo to import %s contains unexpected VkFormat:"
                    "%s [%d]. %s [%d] expected.",
                    importSource.c_str() ?: "", string_VkFormat(imageCreateInfo.format),
                    imageCreateInfo.format, string_VkFormat(colorBufferVkImageCi->format),
                    colorBufferVkImageCi->format);
            }
            if (imageCreateInfo.extent.width != colorBufferVkImageCi->extent.width) {
                ERR("The VkImageCreateInfo to import %s contains unexpected VkExtent::width: "
                    "%" PRIu32 ". %" PRIu32 " expected.",
                    importSource.c_str()?:"", imageCreateInfo.extent.width,
                    colorBufferVkImageCi->extent.width);
            }
            if (imageCreateInfo.extent.height != colorBufferVkImageCi->extent.height) {
                ERR("The VkImageCreateInfo to import %s contains unexpected VkExtent::height: "
                    "%" PRIu32 ". %" PRIu32 " expected.",
                    importSource.c_str()?:"", imageCreateInfo.extent.height,
                    colorBufferVkImageCi->extent.height);
            }
            if (imageCreateInfo.tiling != colorBufferVkImageCi->tiling) {
                ERR("The VkImageCreateInfo to import %s contains unexpected VkImageTiling: %s. %s "
                    "expected.",
                    importSource.c_str()?:"", string_VkImageTiling(imageCreateInfo.tiling),
                    string_VkImageTiling(colorBufferVkImageCi->tiling));
            }
            if (imageCreateInfo.sharingMode != colorBufferVkImageCi->sharingMode) {
                ERR("The VkImageCreateInfo to import %s contains unexpected VkSharingMode: %s. %s "
                    "expected.",
                    importSource.c_str()?:"", string_VkSharingMode(imageCreateInfo.sharingMode),
                    string_VkSharingMode(colorBufferVkImageCi->sharingMode));
            }
        }
    }

    void transformImpl_VkImageCreateInfo_fromhost(const VkImageCreateInfo*, uint32_t) {
        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Not yet implemented.";
    }

#define DEFINE_EXTERNAL_HANDLE_TYPE_TRANSFORM(type, field)                                         \
    void transformImpl_##type##_tohost(const type* props, uint32_t count) {                        \
        type* mut = (type*)props;                                                                  \
        for (uint32_t i = 0; i < count; ++i) {                                                     \
            mut[i].field =                                                                         \
                (VkExternalMemoryHandleTypeFlagBits)transformExternalMemoryHandleTypeFlags_tohost( \
                    mut[i].field);                                                                 \
        }                                                                                          \
    }                                                                                              \
    void transformImpl_##type##_fromhost(const type* props, uint32_t count) {                      \
        type* mut = (type*)props;                                                                  \
        for (uint32_t i = 0; i < count; ++i) {                                                     \
            mut[i].field = (VkExternalMemoryHandleTypeFlagBits)                                    \
                transformExternalMemoryHandleTypeFlags_fromhost(                                   \
                    mut[i].field, GUEST_EXTERNAL_MEMORY_HANDLE_TYPES);                             \
        }                                                                                          \
    }

#define DEFINE_EXTERNAL_MEMORY_PROPERTIES_TRANSFORM(type)                                  \
    void transformImpl_##type##_tohost(const type* props, uint32_t count) {                \
        type* mut = (type*)props;                                                          \
        for (uint32_t i = 0; i < count; ++i) {                                             \
            mut[i].externalMemoryProperties =                                              \
                transformExternalMemoryProperties_tohost(mut[i].externalMemoryProperties); \
        }                                                                                  \
    }                                                                                      \
    void transformImpl_##type##_fromhost(const type* props, uint32_t count) {              \
        type* mut = (type*)props;                                                          \
        for (uint32_t i = 0; i < count; ++i) {                                             \
            mut[i].externalMemoryProperties = transformExternalMemoryProperties_fromhost(  \
                mut[i].externalMemoryProperties, GUEST_EXTERNAL_MEMORY_HANDLE_TYPES);      \
        }                                                                                  \
    }

    DEFINE_EXTERNAL_HANDLE_TYPE_TRANSFORM(VkPhysicalDeviceExternalImageFormatInfo, handleType)
    DEFINE_EXTERNAL_HANDLE_TYPE_TRANSFORM(VkPhysicalDeviceExternalBufferInfo, handleType)
    DEFINE_EXTERNAL_HANDLE_TYPE_TRANSFORM(VkExternalMemoryImageCreateInfo, handleTypes)
    DEFINE_EXTERNAL_HANDLE_TYPE_TRANSFORM(VkExternalMemoryBufferCreateInfo, handleTypes)
    DEFINE_EXTERNAL_HANDLE_TYPE_TRANSFORM(VkExportMemoryAllocateInfo, handleTypes)
    DEFINE_EXTERNAL_MEMORY_PROPERTIES_TRANSFORM(VkExternalImageFormatProperties)
    DEFINE_EXTERNAL_MEMORY_PROPERTIES_TRANSFORM(VkExternalBufferProperties)

    uint64_t newGlobalHandle(const DispatchableHandleInfo<uint64_t>& item,
                             BoxedHandleTypeTag typeTag) {
        if (!mCreatedHandlesForSnapshotLoad.empty() &&
            (mCreatedHandlesForSnapshotLoad.size() - mCreatedHandlesForSnapshotLoadIndex > 0)) {
            auto handle = mCreatedHandlesForSnapshotLoad[mCreatedHandlesForSnapshotLoadIndex];
            VKDGS_LOG("use handle: 0x%lx underlying 0x%lx", handle, item.underlying);
            ++mCreatedHandlesForSnapshotLoadIndex;
            auto res = sBoxedHandleManager.addFixed(handle, item, typeTag);

            return res;
        } else {
            return sBoxedHandleManager.add(item, typeTag);
        }
    }

#define DEFINE_BOXED_DISPATCHABLE_HANDLE_API_IMPL(type)                                           \
    type new_boxed_##type(type underlying, VulkanDispatch* dispatch, bool ownDispatch) {          \
        DispatchableHandleInfo<uint64_t> item;                                                    \
        item.underlying = (uint64_t)underlying;                                                   \
        item.dispatch = dispatch ? dispatch : new VulkanDispatch;                                 \
        item.ownDispatch = ownDispatch;                                                           \
        item.ordMaintInfo = new OrderMaintenanceInfo;                                             \
        item.readStream = nullptr;                                                                \
        auto res = (type)newGlobalHandle(item, Tag_##type);                                       \
        return res;                                                                               \
    }                                                                                             \
    void delete_##type(type boxed) {                                                              \
        if (!boxed) return;                                                                       \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt) return;                                                                         \
        releaseOrderMaintInfo(elt->ordMaintInfo);                                                 \
        if (elt->readStream) {                                                                    \
            sReadStreamRegistry.push(elt->readStream);                                            \
            elt->readStream = nullptr;                                                            \
        }                                                                                         \
        sBoxedHandleManager.remove((uint64_t)boxed);                                              \
    }                                                                                             \
    type unbox_##type(type boxed) {                                                               \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt) return VK_NULL_HANDLE;                                                          \
        return (type)elt->underlying;                                                             \
    }                                                                                             \
    OrderMaintenanceInfo* ordmaint_##type(type boxed) {                                           \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt) return 0;                                                                       \
        auto info = elt->ordMaintInfo;                                                            \
        if (!info) return 0;                                                                      \
        acquireOrderMaintInfo(info);                                                              \
        return info;                                                                              \
    }                                                                                             \
    VulkanMemReadingStream* readstream_##type(type boxed) {                                       \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt) return 0;                                                                       \
        auto stream = elt->readStream;                                                            \
        if (!stream) {                                                                            \
            stream = sReadStreamRegistry.pop(getFeatures());                                      \
            elt->readStream = stream;                                                             \
        }                                                                                         \
        return stream;                                                                            \
    }                                                                                             \
    type unboxed_to_boxed_##type(type unboxed) {                                                  \
        AutoLock lock(sBoxedHandleManager.lock);                                                  \
        return (type)sBoxedHandleManager.getBoxedFromUnboxedLocked((uint64_t)(uintptr_t)unboxed); \
    }                                                                                             \
    VulkanDispatch* dispatch_##type(type boxed) {                                                 \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt) {                                                                               \
            fprintf(stderr, "%s: err not found boxed %p\n", __func__, boxed);                     \
            return nullptr;                                                                       \
        }                                                                                         \
        return elt->dispatch;                                                                     \
    }

#define DEFINE_BOXED_NON_DISPATCHABLE_HANDLE_API_IMPL(type)                                       \
    type new_boxed_non_dispatchable_##type(type underlying) {                                     \
        DispatchableHandleInfo<uint64_t> item;                                                    \
        item.underlying = (uint64_t)underlying;                                                   \
        auto res = (type)newGlobalHandle(item, Tag_##type);                                       \
        return res;                                                                               \
    }                                                                                             \
    void delayed_delete_##type(type boxed, VkDevice device, std::function<void()> callback) {     \
        sBoxedHandleManager.removeDelayed((uint64_t)boxed, device, callback);                     \
    }                                                                                             \
    void delete_##type(type boxed) { sBoxedHandleManager.remove((uint64_t)boxed); }               \
    void set_boxed_non_dispatchable_##type(type boxed, type underlying) {                         \
        DispatchableHandleInfo<uint64_t> item;                                                    \
        item.underlying = (uint64_t)underlying;                                                   \
        sBoxedHandleManager.update((uint64_t)boxed, item, Tag_##type);                            \
    }                                                                                             \
    type unboxed_to_boxed_non_dispatchable_##type(type unboxed) {                                 \
        AutoLock lock(sBoxedHandleManager.lock);                                                  \
        return (type)sBoxedHandleManager.getBoxedFromUnboxedLocked((uint64_t)(uintptr_t)unboxed); \
    }                                                                                             \
    type unbox_##type(type boxed) {                                                               \
        AutoLock lock(sBoxedHandleManager.lock);                                                  \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt) {                                                                               \
            if constexpr (!std::is_same_v<type, VkFence>) {                                       \
                GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))                                   \
                    << "Unbox " << boxed << " failed, not found.";                                \
            }                                                                                     \
            return VK_NULL_HANDLE;                                                                \
        }                                                                                         \
        return (type)elt->underlying;                                                             \
    }

    GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(DEFINE_BOXED_DISPATCHABLE_HANDLE_API_IMPL)
    GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(DEFINE_BOXED_NON_DISPATCHABLE_HANDLE_API_IMPL)

    VkDecoderSnapshot* snapshot() { return &mSnapshot; }
    SnapshotState getSnapshotState() { return mSnapshotState; }

   private:
    bool isEmulatedInstanceExtension(const char* name) const {
        for (auto emulatedExt : kEmulatedInstanceExtensions) {
            if (!strcmp(emulatedExt, name)) return true;
        }
        return false;
    }

    bool isEmulatedDeviceExtension(const char* name) const {
        for (auto emulatedExt : kEmulatedDeviceExtensions) {
            if (!strcmp(emulatedExt, name)) return true;
        }
        return false;
    }

    bool supportEmulatedCompressedImageFormatProperty(VkFormat compressedFormat, VkImageType type,
                                                      VkImageTiling tiling, VkImageUsageFlags usage,
                                                      VkImageCreateFlags flags) {
        // BUG: 139193497
        return !(usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !(type == VK_IMAGE_TYPE_1D);
    }

    std::vector<const char*> filteredDeviceExtensionNames(VulkanDispatch* vk,
                                                          VkPhysicalDevice physicalDevice,
                                                          uint32_t count,
                                                          const char* const* extNames) {
        std::vector<const char*> res;
        std::vector<VkExtensionProperties> properties;
        VkResult result;

        for (uint32_t i = 0; i < count; ++i) {
            auto extName = extNames[i];
            if (!isEmulatedDeviceExtension(extName)) {
                res.push_back(extName);
                continue;
            }
        }

        result = enumerateDeviceExtensionProperties(vk, physicalDevice, nullptr, properties);
        if (result != VK_SUCCESS) {
            VKDGS_LOG("failed to enumerate device extensions");
            return res;
        }

        if (hasDeviceExtension(properties, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)) {
            res.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        }

        if (hasDeviceExtension(properties, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
            res.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        }

        if (hasDeviceExtension(properties, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME)) {
            res.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
        }

        if (hasDeviceExtension(properties, VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME)) {
            res.push_back(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME);
        }

        if (hasDeviceExtension(properties, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            res.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        }

#ifdef _WIN32
        if (hasDeviceExtension(properties, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)) {
            res.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
        }

        if (hasDeviceExtension(properties, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME)) {
            res.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
        }
#elif defined(__QNX__)
        // Note: VK_QNX_external_memory_screen_buffer is not supported in API translation,
        // decoding, etc. However, push name to indicate external memory support to guest
        if (hasDeviceExtension(properties, VK_QNX_EXTERNAL_MEMORY_SCREEN_BUFFER_EXTENSION_NAME)) {
            res.push_back(VK_QNX_EXTERNAL_MEMORY_SCREEN_BUFFER_EXTENSION_NAME);
            // EXT_queue_family_foreign is a pre-requisite for QNX_external_memory_screen_buffer
            if (hasDeviceExtension(properties, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
                res.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
            }
        }

        if (hasDeviceExtension(properties, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
            res.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        }
#elif __unix__
        if (hasDeviceExtension(properties, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
            res.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        }

        if (hasDeviceExtension(properties, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
            res.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        }
#elif defined(__APPLE__)
        if (m_emu->instanceSupportsMoltenVK) {
            if (hasDeviceExtension(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
                res.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
            }
            if (hasDeviceExtension(properties, VK_EXT_METAL_OBJECTS_EXTENSION_NAME)) {
                res.push_back(VK_EXT_METAL_OBJECTS_EXTENSION_NAME);
            }
        } else {
            // Non-MoltenVK path, use memory_fd
            if (hasDeviceExtension(properties, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
                res.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
            }
        }
#endif

#ifdef __linux__
        // A dma-buf is a Linux kernel construct, commonly used with open-source DRM drivers.
        // See https://docs.kernel.org/driver-api/dma-buf.html for details.
        if (m_emu->deviceInfo.supportsDmaBuf &&
            hasDeviceExtension(properties, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
            res.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        }

        if (hasDeviceExtension(properties, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME)) {
            // Mesa Vulkan Wayland WSI needs vkGetImageDrmFormatModifierPropertiesEXT. On some Intel
            // GPUs, this extension is exposed by the driver only if
            // VK_EXT_image_drm_format_modifier extension is requested via
            // VkDeviceCreateInfo::ppEnabledExtensionNames. vkcube-wayland does not request it,
            // which makes the host attempt to call a null function pointer unless we force-enable
            // it regardless of the client's wishes.
            res.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
        }

#endif
        return res;
    }

    std::vector<const char*> filteredInstanceExtensionNames(uint32_t count,
                                                            const char* const* extNames) {
        std::vector<const char*> res;
        for (uint32_t i = 0; i < count; ++i) {
            auto extName = extNames[i];
            if (!isEmulatedInstanceExtension(extName)) {
                res.push_back(extName);
            }
        }

        if (m_emu->instanceSupportsExternalMemoryCapabilities) {
            res.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
        }

        if (m_emu->instanceSupportsExternalSemaphoreCapabilities) {
            res.push_back(VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME);
        }

        if (m_emu->instanceSupportsExternalFenceCapabilities) {
            res.push_back(VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME);
        }

        if (m_emu->debugUtilsAvailableAndRequested) {
            res.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        if (m_emu->instanceSupportsSurface) {
            res.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        }

#if defined(__APPLE__)
        if (m_emu->instanceSupportsMoltenVK) {
            res.push_back(VK_MVK_MACOS_SURFACE_EXTENSION_NAME);
            res.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        }
#endif

        return res;
    }

    bool getDefaultQueueForDeviceLocked(VkDevice device, VkQueue* queue, uint32_t* queueFamilyIndex,
                                        Lock** queueLock) {
        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return false;

        auto zeroIt = deviceInfo->queues.find(0);
        if (zeroIt == deviceInfo->queues.end() || zeroIt->second.empty()) {
            // Get the first queue / queueFamilyIndex
            // that does show up.
            for (const auto& it : deviceInfo->queues) {
                auto index = it.first;
                for (auto& deviceQueue : it.second) {
                    *queue = deviceQueue;
                    *queueFamilyIndex = index;
                    *queueLock = mQueueInfo.at(deviceQueue).lock;
                    return true;
                }
            }
            // Didn't find anything, fail.
            return false;
        } else {
            // Use queue family index 0.
            *queue = zeroIt->second[0];
            *queueFamilyIndex = 0;
            *queueLock = mQueueInfo.at(zeroIt->second[0]).lock;
            return true;
        }

        return false;
    }

    void updateImageMemorySizeLocked(VkDevice device, VkImage image,
                                     VkMemoryRequirements* pMemoryRequirements) {
        auto* deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo->emulateTextureEtc2 && !deviceInfo->emulateTextureAstc) {
            return;
        }
        auto* imageInfo = android::base::find(mImageInfo, image);
        if (!imageInfo) return;
        CompressedImageInfo& cmpInfo = imageInfo->cmpInfo;
        if (!deviceInfo->needEmulatedDecompression(cmpInfo)) {
            return;
        }
        *pMemoryRequirements = cmpInfo.getMemoryRequirements();
    }

    // Whether the VkInstance associated with this physical device was created by ANGLE
    bool isAngleInstance(VkPhysicalDevice physicalDevice, VulkanDispatch* vk) {
        std::lock_guard<std::recursive_mutex> lock(mLock);
        VkInstance* instance = android::base::find(mPhysicalDeviceToInstance, physicalDevice);
        if (!instance) return false;
        InstanceInfo* instanceInfo = android::base::find(mInstanceInfo, *instance);
        if (!instanceInfo) return false;
        return instanceInfo->isAngle;
    }

    bool enableEmulatedEtc2(VkPhysicalDevice physicalDevice, VulkanDispatch* vk) {
        if (!m_emu->enableEtc2Emulation) return false;

        // Don't enable ETC2 emulation for ANGLE, let it do its own emulation.
        return !isAngleInstance(physicalDevice, vk);
    }

    bool enableEmulatedAstc(VkPhysicalDevice physicalDevice, VulkanDispatch* vk) {
        if (m_emu->astcLdrEmulationMode == AstcEmulationMode::Disabled) {
            return false;
        }

        // Don't enable ASTC emulation for ANGLE, let it do its own emulation.
        return !isAngleInstance(physicalDevice, vk);
    }

    bool needEmulatedEtc2(VkPhysicalDevice physicalDevice, VulkanDispatch* vk) {
        if (!enableEmulatedEtc2(physicalDevice, vk)) {
            return false;
        }
        VkPhysicalDeviceFeatures feature;
        vk->vkGetPhysicalDeviceFeatures(physicalDevice, &feature);
        return !feature.textureCompressionETC2;
    }

    bool needEmulatedAstc(VkPhysicalDevice physicalDevice, VulkanDispatch* vk) {
        if (!enableEmulatedAstc(physicalDevice, vk)) {
            return false;
        }
        VkPhysicalDeviceFeatures feature;
        vk->vkGetPhysicalDeviceFeatures(physicalDevice, &feature);
        return !feature.textureCompressionASTC_LDR;
    }

    void getSupportedFenceHandleTypes(VulkanDispatch* vk, VkPhysicalDevice physicalDevice,
                                      uint32_t* supportedFenceHandleTypes) {
        if (!m_emu->instanceSupportsExternalFenceCapabilities) {
            return;
        }

        VkExternalFenceHandleTypeFlagBits handleTypes[] = {
            VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR,
            VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT,
            VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
        };

        for (auto handleType : handleTypes) {
            VkExternalFenceProperties externalFenceProps;
            externalFenceProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES;
            externalFenceProps.pNext = nullptr;

            VkPhysicalDeviceExternalFenceInfo externalFenceInfo = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO, nullptr, handleType};

            vk->vkGetPhysicalDeviceExternalFenceProperties(physicalDevice, &externalFenceInfo,
                                                           &externalFenceProps);

            if ((externalFenceProps.externalFenceFeatures &
                 (VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT)) == 0) {
                continue;
            }

            if ((externalFenceProps.externalFenceFeatures &
                 (VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT)) == 0) {
                continue;
            }

            *supportedFenceHandleTypes |= handleType;
        }
    }

    void getSupportedSemaphoreHandleTypes(VulkanDispatch* vk, VkPhysicalDevice physicalDevice,
                                          uint32_t* supportedBinarySemaphoreHandleTypes) {
        if (!m_emu->instanceSupportsExternalSemaphoreCapabilities) {
            return;
        }

        VkExternalSemaphoreHandleTypeFlagBits handleTypes[] = {
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };

        for (auto handleType : handleTypes) {
            VkExternalSemaphoreProperties externalSemaphoreProps;
            externalSemaphoreProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
            externalSemaphoreProps.pNext = nullptr;

            VkPhysicalDeviceExternalSemaphoreInfo externalSemaphoreInfo = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, nullptr, handleType};

            vk->vkGetPhysicalDeviceExternalSemaphoreProperties(
                physicalDevice, &externalSemaphoreInfo, &externalSemaphoreProps);

            if ((externalSemaphoreProps.externalSemaphoreFeatures &
                 (VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT)) == 0) {
                continue;
            }

            if ((externalSemaphoreProps.externalSemaphoreFeatures &
                 (VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) == 0) {
                continue;
            }

            *supportedBinarySemaphoreHandleTypes |= handleType;
        }
    }

    bool supportsSwapchainMaintenance1(VkPhysicalDevice physicalDevice, VulkanDispatch* vk) {
        bool hasGetPhysicalDeviceFeatures2 = false;
        bool hasGetPhysicalDeviceFeatures2KHR = false;

        {
            std::lock_guard<std::recursive_mutex> lock(mLock);

            auto* physdevInfo = android::base::find(mPhysdevInfo, physicalDevice);
            if (!physdevInfo) {
                return false;
            }

            auto instance = mPhysicalDeviceToInstance[physicalDevice];
            auto* instanceInfo = android::base::find(mInstanceInfo, instance);
            if (!instanceInfo) {
                return false;
            }

            if (instanceInfo->apiVersion >= VK_MAKE_VERSION(1, 1, 0) &&
                physdevInfo->props.apiVersion >= VK_MAKE_VERSION(1, 1, 0)) {
                hasGetPhysicalDeviceFeatures2 = true;
            } else if (hasInstanceExtension(instance,
                                            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
                hasGetPhysicalDeviceFeatures2KHR = true;
            } else {
                return false;
            }
        }

        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchainMaintenance1Features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
            .pNext = nullptr,
            .swapchainMaintenance1 = VK_FALSE,
        };
        VkPhysicalDeviceFeatures2 features2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &swapchainMaintenance1Features,
        };
        if (hasGetPhysicalDeviceFeatures2) {
            vk->vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
        } else if (hasGetPhysicalDeviceFeatures2KHR) {
            vk->vkGetPhysicalDeviceFeatures2KHR(physicalDevice, &features2);
        } else {
            return false;
        }

        return swapchainMaintenance1Features.swapchainMaintenance1 == VK_TRUE;
    }

    bool isEmulatedCompressedTexture(VkFormat format, VkPhysicalDevice physicalDevice,
                                     VulkanDispatch* vk) {
        return (gfxstream::vk::isEtc2(format) && needEmulatedEtc2(physicalDevice, vk)) ||
               (gfxstream::vk::isAstc(format) && needEmulatedAstc(physicalDevice, vk));
    }

    static const VkFormatFeatureFlags kEmulatedTextureBufferFeatureMask =
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

    static const VkFormatFeatureFlags kEmulatedTextureOptimalTilingMask =
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

    void maskFormatPropertiesForEmulatedTextures(VkFormatProperties* pFormatProp) {
        pFormatProp->linearTilingFeatures &= kEmulatedTextureBufferFeatureMask;
        pFormatProp->optimalTilingFeatures &= kEmulatedTextureOptimalTilingMask;
        pFormatProp->bufferFeatures &= kEmulatedTextureBufferFeatureMask;
    }

    void maskFormatPropertiesForEmulatedTextures(VkFormatProperties2* pFormatProp) {
        pFormatProp->formatProperties.linearTilingFeatures &= kEmulatedTextureBufferFeatureMask;
        pFormatProp->formatProperties.optimalTilingFeatures &= kEmulatedTextureOptimalTilingMask;
        pFormatProp->formatProperties.bufferFeatures &= kEmulatedTextureBufferFeatureMask;
    }

    void maskImageFormatPropertiesForEmulatedTextures(VkImageFormatProperties* pProperties) {
        // dEQP-VK.api.info.image_format_properties.2d.optimal#etc2_r8g8b8_unorm_block
        pProperties->sampleCounts &= VK_SAMPLE_COUNT_1_BIT;
    }

    template <class VkFormatProperties1or2>
    void getPhysicalDeviceFormatPropertiesCore(
        std::function<void(VkPhysicalDevice, VkFormat, VkFormatProperties1or2*)>
            getPhysicalDeviceFormatPropertiesFunc,
        VulkanDispatch* vk, VkPhysicalDevice physicalDevice, VkFormat format,
        VkFormatProperties1or2* pFormatProperties) {
        if (isEmulatedCompressedTexture(format, physicalDevice, vk)) {
            getPhysicalDeviceFormatPropertiesFunc(
                physicalDevice, CompressedImageInfo::getOutputFormat(format),
                pFormatProperties);
            maskFormatPropertiesForEmulatedTextures(pFormatProperties);
            return;
        }
        getPhysicalDeviceFormatPropertiesFunc(physicalDevice, format, pFormatProperties);
    }

    void executePreprocessRecursive(int level, VkCommandBuffer cmdBuffer) {
        auto* cmdBufferInfo = android::base::find(mCommandBufferInfo, cmdBuffer);
        if (!cmdBufferInfo) return;
        for (const auto& func : cmdBufferInfo->preprocessFuncs) {
            func();
        }
        // TODO: fix
        // for (const auto& subCmd : cmdBufferInfo->subCmds) {
        // executePreprocessRecursive(level + 1, subCmd);
        // }
    }

    void executePreprocessRecursive(const VkSubmitInfo& submit) {
        for (uint32_t c = 0; c < submit.commandBufferCount; c++) {
            executePreprocessRecursive(0, submit.pCommandBuffers[c]);
        }
    }

    void executePreprocessRecursive(const VkSubmitInfo2& submit) {
        for (uint32_t c = 0; c < submit.commandBufferInfoCount; c++) {
            executePreprocessRecursive(0, submit.pCommandBufferInfos[c].commandBuffer);
        }
    }

    template <typename VkHandleToInfoMap,
              typename HandleType = typename std::decay_t<VkHandleToInfoMap>::key_type>
    std::vector<HandleType> findDeviceObjects(VkDevice device, const VkHandleToInfoMap& map) {
        std::vector<HandleType> objectsFromDevice;
        for (const auto& [objectHandle, objectInfo] : map) {
            if (objectInfo.device == device) {
                objectsFromDevice.push_back(objectHandle);
            }
        }
        return objectsFromDevice;
    }

    template <typename VkHandleToInfoMap, typename InfoMemberType,
              typename HandleType = typename std::decay_t<VkHandleToInfoMap>::key_type,
              typename InfoType = typename std::decay_t<VkHandleToInfoMap>::value_type>
    std::vector<std::pair<HandleType, InfoMemberType>> findDeviceObjects(
        VkDevice device, const VkHandleToInfoMap& map, InfoMemberType InfoType::*member) {
        std::vector<std::pair<HandleType, InfoMemberType>> objectsFromDevice;
        for (const auto& [objectHandle, objectInfo] : map) {
            if (objectInfo.device == device) {
                objectsFromDevice.emplace_back(objectHandle, objectInfo.*member);
            }
        }
        return objectsFromDevice;
    }

    void teardownInstanceLocked(VkInstance instance) {
        std::vector<VkDevice> devicesToDestroy;
        std::vector<VulkanDispatch*> devicesToDestroyDispatches;

        for (auto it : mDeviceToPhysicalDevice) {
            auto* otherInstance = android::base::find(mPhysicalDeviceToInstance, it.second);
            if (!otherInstance) continue;

            if (instance == *otherInstance) {
                devicesToDestroy.push_back(it.first);
                devicesToDestroyDispatches.push_back(
                    dispatch_VkDevice(mDeviceInfo[it.first].boxed));
            }
        }

        for (uint32_t i = 0; i < devicesToDestroy.size(); ++i) {
            VkDevice deviceToDestroy = devicesToDestroy[i];
            VulkanDispatch* deviceToDestroyDispatch = devicesToDestroyDispatches[i];

            // https://bugs.chromium.org/p/chromium/issues/detail?id=1074600
            // it's important to idle the device before destroying it!
            deviceToDestroyDispatch->vkDeviceWaitIdle(deviceToDestroy);

            for (auto semaphore : findDeviceObjects(deviceToDestroy, mSemaphoreInfo)) {
                destroySemaphoreLocked(deviceToDestroy, deviceToDestroyDispatch, semaphore,
                                       nullptr);
            }

            for (auto sampler : findDeviceObjects(deviceToDestroy, mSamplerInfo)) {
                destroySamplerLocked(deviceToDestroy, deviceToDestroyDispatch, sampler, nullptr);
            }

            for (auto buffer : findDeviceObjects(deviceToDestroy, mBufferInfo)) {
                deviceToDestroyDispatch->vkDestroyBuffer(deviceToDestroy, buffer, nullptr);
                mBufferInfo.erase(buffer);
            }

            for (auto imageView : findDeviceObjects(deviceToDestroy, mImageViewInfo)) {
                deviceToDestroyDispatch->vkDestroyImageView(deviceToDestroy, imageView, nullptr);
                mImageViewInfo.erase(imageView);
            }

            for (auto image : findDeviceObjects(deviceToDestroy, mImageInfo)) {
                destroyImageLocked(deviceToDestroy, deviceToDestroyDispatch, image, nullptr);
            }

            for (auto memory : findDeviceObjects(deviceToDestroy, mMemoryInfo)) {
                freeMemoryLocked(deviceToDestroyDispatch, deviceToDestroy, memory, nullptr);
            }

            for (auto [commandBuffer, commandPool] : findDeviceObjects(
                     deviceToDestroy, mCommandBufferInfo, &CommandBufferInfo::cmdPool)) {
                // The command buffer is freed with the vkDestroyCommandPool() below.
                delete_VkCommandBuffer(unboxed_to_boxed_VkCommandBuffer(commandBuffer));
                mCommandBufferInfo.erase(commandBuffer);
            }

            for (auto [commandPool, commandPoolBoxed] :
                 findDeviceObjects(deviceToDestroy, mCommandPoolInfo, &CommandPoolInfo::boxed)) {
                deviceToDestroyDispatch->vkDestroyCommandPool(deviceToDestroy, commandPool,
                                                              nullptr);
                delete_VkCommandPool(commandPoolBoxed);
                mCommandPoolInfo.erase(commandPool);
            }

            for (auto [descriptorPool, descriptorPoolBoxed] : findDeviceObjects(
                     deviceToDestroy, mDescriptorPoolInfo, &DescriptorPoolInfo::boxed)) {
                cleanupDescriptorPoolAllocedSetsLocked(descriptorPool, /*isDestroy=*/true);
                deviceToDestroyDispatch->vkDestroyDescriptorPool(deviceToDestroy, descriptorPool,
                                                                 nullptr);
                delete_VkDescriptorPool(descriptorPoolBoxed);
                mDescriptorPoolInfo.erase(descriptorPool);
            }

            for (auto [descriptorSetLayout, descriptorSetLayoutBoxed] : findDeviceObjects(
                     deviceToDestroy, mDescriptorSetLayoutInfo, &DescriptorSetLayoutInfo::boxed)) {
                deviceToDestroyDispatch->vkDestroyDescriptorSetLayout(deviceToDestroy,
                                                                      descriptorSetLayout, nullptr);
                delete_VkDescriptorSetLayout(descriptorSetLayoutBoxed);
                mDescriptorSetLayoutInfo.erase(descriptorSetLayout);
            }

            for (auto shaderModule : findDeviceObjects(deviceToDestroy, mShaderModuleInfo)) {
                destroyShaderModuleLocked(deviceToDestroy, deviceToDestroyDispatch, shaderModule,
                                          nullptr);
            }

            for (auto pipeline : findDeviceObjects(deviceToDestroy, mPipelineInfo)) {
                destroyPipelineLocked(deviceToDestroy, deviceToDestroyDispatch, pipeline, nullptr);
            }

            for (auto pipelineCache : findDeviceObjects(deviceToDestroy, mPipelineCacheInfo)) {
                destroyPipelineCacheLocked(deviceToDestroy, deviceToDestroyDispatch, pipelineCache,
                                           nullptr);
            }

            for (auto framebuffer : findDeviceObjects(deviceToDestroy, mFramebufferInfo)) {
                destroyFramebufferLocked(deviceToDestroy, deviceToDestroyDispatch, framebuffer,
                                         nullptr);
            }

            for (auto renderPass : findDeviceObjects(deviceToDestroy, mRenderPassInfo)) {
                destroyRenderPassLocked(deviceToDestroy, deviceToDestroyDispatch, renderPass,
                                        nullptr);
            }
        }

        for (VkDevice deviceToDestroy : devicesToDestroy) {
            destroyDeviceLocked(deviceToDestroy, nullptr);
            mDeviceInfo.erase(deviceToDestroy);
            mDeviceToPhysicalDevice.erase(deviceToDestroy);
        }

        // TODO: Clean up the physical device info in `mPhysdevInfo` but we need to be careful
        // as the Vulkan spec does not guarantee that the VkPhysicalDevice handles returned are
        // unique per VkInstance.
    }

    typedef std::function<void()> PreprocessFunc;
    struct CommandBufferInfo {
        std::vector<PreprocessFunc> preprocessFuncs = {};
        std::vector<VkCommandBuffer> subCmds = {};
        VkDevice device = VK_NULL_HANDLE;
        VkCommandPool cmdPool = VK_NULL_HANDLE;
        VkCommandBuffer boxed = VK_NULL_HANDLE;
        DebugUtilsHelper debugUtilsHelper = DebugUtilsHelper::withUtilsDisabled();

        // Most recently bound compute pipeline and descriptor sets. We save it here so that we can
        // restore it after doing emulated texture decompression.
        VkPipeline computePipeline = VK_NULL_HANDLE;
        uint32_t firstSet = 0;
        VkPipelineLayout descriptorLayout = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> currentDescriptorSets;
        std::unordered_set<VkDescriptorSet> allDescriptorSets;
        std::vector<uint32_t> dynamicOffsets;
        std::unordered_set<HandleType> acquiredColorBuffers;
        std::unordered_set<HandleType> releasedColorBuffers;
        std::unordered_map<HandleType, VkImageLayout> cbLayouts;
        std::unordered_map<VkImage, VkImageLayout> imageLayouts;
        std::unordered_set<HandleType> imageBarrierColorBuffers;

        void reset() {
            preprocessFuncs.clear();
            subCmds.clear();
            computePipeline = VK_NULL_HANDLE;
            firstSet = 0;
            descriptorLayout = VK_NULL_HANDLE;
            currentDescriptorSets.clear();
            allDescriptorSets.clear();
            dynamicOffsets.clear();
            acquiredColorBuffers.clear();
            releasedColorBuffers.clear();
            cbLayouts.clear();
            imageLayouts.clear();
        }
    };

    struct CommandPoolInfo {
        VkDevice device = VK_NULL_HANDLE;
        VkCommandPool boxed = VK_NULL_HANDLE;
        std::unordered_set<VkCommandBuffer> cmdBuffers = {};
    };

    void removeCommandBufferInfo(const std::unordered_set<VkCommandBuffer>& cmdBuffers) {
        for (const auto& cmdBuffer : cmdBuffers) {
            mCommandBufferInfo.erase(cmdBuffer);
        }
    }

    bool isDescriptorTypeImageInfo(VkDescriptorType descType) {
        return (descType == VK_DESCRIPTOR_TYPE_SAMPLER) ||
               (descType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ||
               (descType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) ||
               (descType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ||
               (descType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
    }

    bool descriptorTypeContainsImage(VkDescriptorType descType) {
        return (descType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ||
               (descType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) ||
               (descType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ||
               (descType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
    }

    bool descriptorTypeContainsSampler(VkDescriptorType descType) {
        return (descType == VK_DESCRIPTOR_TYPE_SAMPLER) ||
               (descType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    }

    bool isDescriptorTypeBufferInfo(VkDescriptorType descType) {
        return (descType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) ||
               (descType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) ||
               (descType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) ||
               (descType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
    }

    bool isDescriptorTypeBufferView(VkDescriptorType descType) {
        return (descType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) ||
               (descType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
    }

    bool isDescriptorTypeInlineUniformBlock(VkDescriptorType descType) {
        return descType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT;
    }

    bool isDescriptorTypeAccelerationStructure(VkDescriptorType descType) {
        return descType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    }

    int descriptorDependencyObjectCount(VkDescriptorType descType) {
        switch (descType) {
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                return 2;
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                return 1;
            default:
                return 0;
        }
    }

    struct DescriptorUpdateTemplateInfo {
        VkDescriptorUpdateTemplateCreateInfo createInfo;
        std::vector<VkDescriptorUpdateTemplateEntry> linearizedTemplateEntries;
        // Preallocated pData
        std::vector<uint8_t> data;
        size_t imageInfoStart;
        size_t bufferInfoStart;
        size_t bufferViewStart;
        size_t inlineUniformBlockStart;
    };

    DescriptorUpdateTemplateInfo calcLinearizedDescriptorUpdateTemplateInfo(
        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo) {
        DescriptorUpdateTemplateInfo res;
        res.createInfo = *pCreateInfo;

        size_t numImageInfos = 0;
        size_t numBufferInfos = 0;
        size_t numBufferViews = 0;
        size_t numInlineUniformBlocks = 0;

        for (uint32_t i = 0; i < pCreateInfo->descriptorUpdateEntryCount; ++i) {
            const auto& entry = pCreateInfo->pDescriptorUpdateEntries[i];
            auto type = entry.descriptorType;
            auto count = entry.descriptorCount;
            if (isDescriptorTypeImageInfo(type)) {
                numImageInfos += count;
            } else if (isDescriptorTypeBufferInfo(type)) {
                numBufferInfos += count;
            } else if (isDescriptorTypeBufferView(type)) {
                numBufferViews += count;
            } else if (type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT) {
                numInlineUniformBlocks += count;
            } else {
                GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                    << "unknown descriptor type 0x" << std::hex << type;
            }
        }

        size_t imageInfoBytes = numImageInfos * sizeof(VkDescriptorImageInfo);
        size_t bufferInfoBytes = numBufferInfos * sizeof(VkDescriptorBufferInfo);
        size_t bufferViewBytes = numBufferViews * sizeof(VkBufferView);
        size_t inlineUniformBlockBytes = numInlineUniformBlocks;

        res.data.resize(imageInfoBytes + bufferInfoBytes + bufferViewBytes +
                        inlineUniformBlockBytes);
        res.imageInfoStart = 0;
        res.bufferInfoStart = imageInfoBytes;
        res.bufferViewStart = imageInfoBytes + bufferInfoBytes;
        res.inlineUniformBlockStart = imageInfoBytes + bufferInfoBytes + bufferViewBytes;

        size_t imageInfoCount = 0;
        size_t bufferInfoCount = 0;
        size_t bufferViewCount = 0;
        size_t inlineUniformBlockCount = 0;

        for (uint32_t i = 0; i < pCreateInfo->descriptorUpdateEntryCount; ++i) {
            const auto& entry = pCreateInfo->pDescriptorUpdateEntries[i];
            VkDescriptorUpdateTemplateEntry entryForHost = entry;

            auto type = entry.descriptorType;

            if (isDescriptorTypeImageInfo(type)) {
                entryForHost.offset =
                    res.imageInfoStart + imageInfoCount * sizeof(VkDescriptorImageInfo);
                entryForHost.stride = sizeof(VkDescriptorImageInfo);
                ++imageInfoCount;
            } else if (isDescriptorTypeBufferInfo(type)) {
                entryForHost.offset =
                    res.bufferInfoStart + bufferInfoCount * sizeof(VkDescriptorBufferInfo);
                entryForHost.stride = sizeof(VkDescriptorBufferInfo);
                ++bufferInfoCount;
            } else if (isDescriptorTypeBufferView(type)) {
                entryForHost.offset = res.bufferViewStart + bufferViewCount * sizeof(VkBufferView);
                entryForHost.stride = sizeof(VkBufferView);
                ++bufferViewCount;
            } else if (type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT) {
                entryForHost.offset = res.inlineUniformBlockStart + inlineUniformBlockCount;
                entryForHost.stride = 0;
                inlineUniformBlockCount += entryForHost.descriptorCount;
            } else {
                GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))
                    << "unknown descriptor type 0x" << std::hex << type;
            }

            res.linearizedTemplateEntries.push_back(entryForHost);
        }

        res.createInfo.pDescriptorUpdateEntries = res.linearizedTemplateEntries.data();

        return res;
    }

    void registerDescriptorUpdateTemplate(VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                          const DescriptorUpdateTemplateInfo& info) {
        std::lock_guard<std::recursive_mutex> lock(mLock);
        mDescriptorUpdateTemplateInfo[descriptorUpdateTemplate] = info;
    }

    void unregisterDescriptorUpdateTemplate(VkDescriptorUpdateTemplate descriptorUpdateTemplate) {
        std::lock_guard<std::recursive_mutex> lock(mLock);
        mDescriptorUpdateTemplateInfo.erase(descriptorUpdateTemplate);
    }

    // Returns the VkInstance associated with a VkDevice, or null if it's not found
    VkInstance* deviceToInstanceLocked(VkDevice device) {
        auto* physicalDevice = android::base::find(mDeviceToPhysicalDevice, device);
        if (!physicalDevice) return nullptr;
        return android::base::find(mPhysicalDeviceToInstance, *physicalDevice);
    }

    VulkanDispatch* m_vk;
    VkEmulation* m_emu;
    emugl::RenderDocWithMultipleVkInstances* mRenderDocWithMultipleVkInstances = nullptr;
    bool mSnapshotsEnabled = false;
    bool mVkCleanupEnabled = true;
    bool mLogging = false;
    bool mVerbosePrints = false;
    bool mUseOldMemoryCleanupPath = false;

    std::recursive_mutex mLock;

    bool isBindingFeasibleForAlloc(const DescriptorPoolInfo::PoolState& poolState,
                                   const VkDescriptorSetLayoutBinding& binding) {
        if (binding.descriptorCount && (poolState.type != binding.descriptorType)) {
            return false;
        }

        uint32_t availDescriptorCount = poolState.descriptorCount - poolState.used;

        if (availDescriptorCount < binding.descriptorCount) {
            return false;
        }

        return true;
    }

    bool isBindingFeasibleForFree(const DescriptorPoolInfo::PoolState& poolState,
                                  const VkDescriptorSetLayoutBinding& binding) {
        if (poolState.type != binding.descriptorType) return false;
        if (poolState.used < binding.descriptorCount) return false;
        return true;
    }

    void allocBindingFeasible(const VkDescriptorSetLayoutBinding& binding,
                              DescriptorPoolInfo::PoolState& poolState) {
        poolState.used += binding.descriptorCount;
    }

    void freeBindingFeasible(const VkDescriptorSetLayoutBinding& binding,
                             DescriptorPoolInfo::PoolState& poolState) {
        poolState.used -= binding.descriptorCount;
    }

    VkResult validateDescriptorSetAllocLocked(const VkDescriptorSetAllocateInfo* pAllocateInfo) {
        auto* poolInfo = android::base::find(mDescriptorPoolInfo, pAllocateInfo->descriptorPool);
        if (!poolInfo) return VK_ERROR_INITIALIZATION_FAILED;

        // Check the number of sets available.
        auto setsAvailable = poolInfo->maxSets - poolInfo->usedSets;

        if (setsAvailable < pAllocateInfo->descriptorSetCount) {
            return VK_ERROR_OUT_OF_POOL_MEMORY;
        }

        // Perform simulated allocation and error out with
        // VK_ERROR_OUT_OF_POOL_MEMORY if it fails.
        std::vector<DescriptorPoolInfo::PoolState> poolCopy = poolInfo->pools;

        for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; ++i) {
            auto setLayoutInfo =
                android::base::find(mDescriptorSetLayoutInfo, pAllocateInfo->pSetLayouts[i]);
            if (!setLayoutInfo) return VK_ERROR_INITIALIZATION_FAILED;

            for (const auto& binding : setLayoutInfo->bindings) {
                bool success = false;
                for (auto& pool : poolCopy) {
                    if (!isBindingFeasibleForAlloc(pool, binding)) continue;

                    success = true;
                    allocBindingFeasible(binding, pool);
                    break;
                }

                if (!success) {
                    return VK_ERROR_OUT_OF_POOL_MEMORY;
                }
            }
        }
        return VK_SUCCESS;
    }

    void applyDescriptorSetAllocationLocked(
        DescriptorPoolInfo& poolInfo, const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
        ++poolInfo.usedSets;
        for (const auto& binding : bindings) {
            for (auto& pool : poolInfo.pools) {
                if (!isBindingFeasibleForAlloc(pool, binding)) continue;
                allocBindingFeasible(binding, pool);
                break;
            }
        }
    }

    void removeDescriptorSetAllocationLocked(
        DescriptorPoolInfo& poolInfo, const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
        --poolInfo.usedSets;
        for (const auto& binding : bindings) {
            for (auto& pool : poolInfo.pools) {
                if (!isBindingFeasibleForFree(pool, binding)) continue;
                freeBindingFeasible(binding, pool);
                break;
            }
        }
    }

    template <class T>
    class NonDispatchableHandleInfo {
       public:
        T underlying;
    };

    std::unordered_map<VkInstance, InstanceInfo> mInstanceInfo;
    std::unordered_map<VkPhysicalDevice, PhysicalDeviceInfo> mPhysdevInfo;
    std::unordered_map<VkDevice, DeviceInfo> mDeviceInfo;
    std::unordered_map<VkImage, ImageInfo> mImageInfo;
    std::unordered_map<VkImageView, ImageViewInfo> mImageViewInfo;
    std::unordered_map<VkSampler, SamplerInfo> mSamplerInfo;
    std::unordered_map<VkCommandBuffer, CommandBufferInfo> mCommandBufferInfo;
    std::unordered_map<VkCommandPool, CommandPoolInfo> mCommandPoolInfo;
    // TODO: release CommandBufferInfo when a command pool is reset/released

    // Back-reference to the physical device associated with a particular
    // VkDevice, and the VkDevice corresponding to a VkQueue.
    std::unordered_map<VkDevice, VkPhysicalDevice> mDeviceToPhysicalDevice;
    std::unordered_map<VkPhysicalDevice, VkInstance> mPhysicalDeviceToInstance;

    std::unordered_map<VkQueue, QueueInfo> mQueueInfo;
    std::unordered_map<VkBuffer, BufferInfo> mBufferInfo;

    std::unordered_map<VkDeviceMemory, MemoryInfo> mMemoryInfo;

    std::unordered_map<VkShaderModule, ShaderModuleInfo> mShaderModuleInfo;
    std::unordered_map<VkPipelineCache, PipelineCacheInfo> mPipelineCacheInfo;
    std::unordered_map<VkPipeline, PipelineInfo> mPipelineInfo;
    std::unordered_map<VkRenderPass, RenderPassInfo> mRenderPassInfo;
    std::unordered_map<VkFramebuffer, FramebufferInfo> mFramebufferInfo;

    std::unordered_map<VkSemaphore, SemaphoreInfo> mSemaphoreInfo;
    std::unordered_map<VkFence, FenceInfo> mFenceInfo;

    std::unordered_map<VkDescriptorSetLayout, DescriptorSetLayoutInfo> mDescriptorSetLayoutInfo;
    std::unordered_map<VkDescriptorPool, DescriptorPoolInfo> mDescriptorPoolInfo;
    std::unordered_map<VkDescriptorSet, DescriptorSetInfo> mDescriptorSetInfo;

#ifdef _WIN32
    int mSemaphoreId = 1;
    int genSemaphoreId() {
        if (mSemaphoreId == -1) {
            mSemaphoreId = 1;
        }
        int res = mSemaphoreId;
        ++mSemaphoreId;
        return res;
    }
    std::unordered_map<int, VkSemaphore> mExternalSemaphoresById;
#endif
    std::unordered_map<VkDescriptorUpdateTemplate, DescriptorUpdateTemplateInfo>
        mDescriptorUpdateTemplateInfo;

    VkDecoderSnapshot mSnapshot;

    std::vector<uint64_t> mCreatedHandlesForSnapshotLoad;
    size_t mCreatedHandlesForSnapshotLoadIndex = 0;

    Lock mOccupiedGpasLock;
    // Back-reference to the VkDeviceMemory that is occupying a particular
    // guest physical address
    struct OccupiedGpaInfo {
        VulkanDispatch* vk;
        VkDevice device;
        VkDeviceMemory memory;
        uint64_t gpa;
        size_t sizeToPage;
    };
    std::unordered_map<uint64_t, OccupiedGpaInfo> mOccupiedGpas;

    struct LinearImageCreateInfo {
        VkExtent3D extent;
        VkFormat format;
        VkImageUsageFlags usage;

        VkImageCreateInfo toDefaultVk() const {
            return VkImageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = {},
                .imageType = VK_IMAGE_TYPE_2D,
                .format = format,
                .extent = extent,
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_LINEAR,
                .usage = usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
        }

        struct Hash {
            std::size_t operator()(const LinearImageCreateInfo& ci) const {
                std::size_t s = 0;
                // Magic number used in boost::hash_combine().
                constexpr size_t kHashMagic = 0x9e3779b9;
                s ^= std::hash<uint32_t>{}(ci.extent.width) + kHashMagic + (s << 6) + (s >> 2);
                s ^= std::hash<uint32_t>{}(ci.extent.height) + kHashMagic + (s << 6) + (s >> 2);
                s ^= std::hash<uint32_t>{}(ci.extent.depth) + kHashMagic + (s << 6) + (s >> 2);
                s ^= std::hash<VkFormat>{}(ci.format) + kHashMagic + (s << 6) + (s >> 2);
                s ^= std::hash<VkImageUsageFlags>{}(ci.usage) + kHashMagic + (s << 6) + (s >> 2);
                return s;
            }
        };
    };

    friend bool operator==(const LinearImageCreateInfo& a, const LinearImageCreateInfo& b) {
        return a.extent.width == b.extent.width && a.extent.height == b.extent.height &&
               a.extent.depth == b.extent.depth && a.format == b.format && a.usage == b.usage;
    }

    struct LinearImageProperties {
        VkDeviceSize offset;
        VkDeviceSize rowPitchAlignment;
    };

    // TODO(liyl): Remove after removing the old vkGetLinearImageLayoutGOOGLE.
    std::unordered_map<VkFormat, LinearImageProperties> mPerFormatLinearImageProperties;

    std::unordered_map<LinearImageCreateInfo, LinearImageProperties, LinearImageCreateInfo::Hash>
        mLinearImageProperties;

    SnapshotState mSnapshotState = SnapshotState::Normal;
};

VkDecoderGlobalState::VkDecoderGlobalState() : mImpl(new VkDecoderGlobalState::Impl()) {}

VkDecoderGlobalState::~VkDecoderGlobalState() = default;

static VkDecoderGlobalState* sGlobalDecoderState = nullptr;

// static
VkDecoderGlobalState* VkDecoderGlobalState::get() {
    if (sGlobalDecoderState) return sGlobalDecoderState;
    sGlobalDecoderState = new VkDecoderGlobalState;
    return sGlobalDecoderState;
}

// static
void VkDecoderGlobalState::reset() {
    delete sGlobalDecoderState;
    sGlobalDecoderState = nullptr;
}

// Snapshots
bool VkDecoderGlobalState::snapshotsEnabled() const { return mImpl->snapshotsEnabled(); }

VkDecoderGlobalState::SnapshotState VkDecoderGlobalState::getSnapshotState() const {
    return mImpl->getSnapshotState();
}

const gfxstream::host::FeatureSet& VkDecoderGlobalState::getFeatures() const { return mImpl->getFeatures(); }

bool VkDecoderGlobalState::vkCleanupEnabled() const { return mImpl->vkCleanupEnabled(); }

void VkDecoderGlobalState::save(android::base::Stream* stream) { mImpl->save(stream); }

void VkDecoderGlobalState::load(android::base::Stream* stream, GfxApiLogger& gfxLogger,
                                HealthMonitor<>* healthMonitor) {
    mImpl->load(stream, gfxLogger, healthMonitor);
}

void VkDecoderGlobalState::lock() { mImpl->lock(); }

void VkDecoderGlobalState::unlock() { mImpl->unlock(); }

size_t VkDecoderGlobalState::setCreatedHandlesForSnapshotLoad(const unsigned char* buffer) {
    return mImpl->setCreatedHandlesForSnapshotLoad(buffer);
}

void VkDecoderGlobalState::clearCreatedHandlesForSnapshotLoad() {
    mImpl->clearCreatedHandlesForSnapshotLoad();
}

VkResult VkDecoderGlobalState::on_vkEnumerateInstanceVersion(android::base::BumpPool* pool,
                                                             uint32_t* pApiVersion) {
    return mImpl->on_vkEnumerateInstanceVersion(pool, pApiVersion);
}

VkResult VkDecoderGlobalState::on_vkCreateInstance(android::base::BumpPool* pool,
                                                   const VkInstanceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator,
                                                   VkInstance* pInstance) {
    return mImpl->on_vkCreateInstance(pool, pCreateInfo, pAllocator, pInstance);
}

void VkDecoderGlobalState::on_vkDestroyInstance(android::base::BumpPool* pool, VkInstance instance,
                                                const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyInstance(pool, instance, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkEnumeratePhysicalDevices(android::base::BumpPool* pool,
                                                             VkInstance instance,
                                                             uint32_t* physicalDeviceCount,
                                                             VkPhysicalDevice* physicalDevices) {
    return mImpl->on_vkEnumeratePhysicalDevices(pool, instance, physicalDeviceCount,
                                                physicalDevices);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceFeatures(android::base::BumpPool* pool,
                                                          VkPhysicalDevice physicalDevice,
                                                          VkPhysicalDeviceFeatures* pFeatures) {
    mImpl->on_vkGetPhysicalDeviceFeatures(pool, physicalDevice, pFeatures);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceFeatures2(android::base::BumpPool* pool,
                                                           VkPhysicalDevice physicalDevice,
                                                           VkPhysicalDeviceFeatures2* pFeatures) {
    mImpl->on_vkGetPhysicalDeviceFeatures2(pool, physicalDevice, pFeatures);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceFeatures2KHR(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2KHR* pFeatures) {
    mImpl->on_vkGetPhysicalDeviceFeatures2(pool, physicalDevice, pFeatures);
}

VkResult VkDecoderGlobalState::on_vkGetPhysicalDeviceImageFormatProperties(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice, VkFormat format,
    VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
    VkImageFormatProperties* pImageFormatProperties) {
    return mImpl->on_vkGetPhysicalDeviceImageFormatProperties(
        pool, physicalDevice, format, type, tiling, usage, flags, pImageFormatProperties);
}
VkResult VkDecoderGlobalState::on_vkGetPhysicalDeviceImageFormatProperties2(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
    VkImageFormatProperties2* pImageFormatProperties) {
    return mImpl->on_vkGetPhysicalDeviceImageFormatProperties2(
        pool, physicalDevice, pImageFormatInfo, pImageFormatProperties);
}
VkResult VkDecoderGlobalState::on_vkGetPhysicalDeviceImageFormatProperties2KHR(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
    VkImageFormatProperties2* pImageFormatProperties) {
    return mImpl->on_vkGetPhysicalDeviceImageFormatProperties2(
        pool, physicalDevice, pImageFormatInfo, pImageFormatProperties);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceFormatProperties(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice, VkFormat format,
    VkFormatProperties* pFormatProperties) {
    mImpl->on_vkGetPhysicalDeviceFormatProperties(pool, physicalDevice, format, pFormatProperties);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceFormatProperties2(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice, VkFormat format,
    VkFormatProperties2* pFormatProperties) {
    mImpl->on_vkGetPhysicalDeviceFormatProperties2(pool, physicalDevice, format, pFormatProperties);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceFormatProperties2KHR(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice, VkFormat format,
    VkFormatProperties2* pFormatProperties) {
    mImpl->on_vkGetPhysicalDeviceFormatProperties2(pool, physicalDevice, format, pFormatProperties);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceProperties(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties) {
    mImpl->on_vkGetPhysicalDeviceProperties(pool, physicalDevice, pProperties);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceProperties2(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties) {
    mImpl->on_vkGetPhysicalDeviceProperties2(pool, physicalDevice, pProperties);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceProperties2KHR(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties) {
    mImpl->on_vkGetPhysicalDeviceProperties2(pool, physicalDevice, pProperties);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceMemoryProperties(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    mImpl->on_vkGetPhysicalDeviceMemoryProperties(pool, physicalDevice, pMemoryProperties);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceMemoryProperties2(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
    mImpl->on_vkGetPhysicalDeviceMemoryProperties2(pool, physicalDevice, pMemoryProperties);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceMemoryProperties2KHR(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
    mImpl->on_vkGetPhysicalDeviceMemoryProperties2(pool, physicalDevice, pMemoryProperties);
}

VkResult VkDecoderGlobalState::on_vkEnumerateDeviceExtensionProperties(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice, const char* pLayerName,
    uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    return mImpl->on_vkEnumerateDeviceExtensionProperties(pool, physicalDevice, pLayerName,
                                                          pPropertyCount, pProperties);
}

VkResult VkDecoderGlobalState::on_vkCreateDevice(android::base::BumpPool* pool,
                                                 VkPhysicalDevice physicalDevice,
                                                 const VkDeviceCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkDevice* pDevice) {
    return mImpl->on_vkCreateDevice(pool, physicalDevice, pCreateInfo, pAllocator, pDevice);
}

void VkDecoderGlobalState::on_vkGetDeviceQueue(android::base::BumpPool* pool, VkDevice device,
                                               uint32_t queueFamilyIndex, uint32_t queueIndex,
                                               VkQueue* pQueue) {
    mImpl->on_vkGetDeviceQueue(pool, device, queueFamilyIndex, queueIndex, pQueue);
}

void VkDecoderGlobalState::on_vkGetDeviceQueue2(android::base::BumpPool* pool, VkDevice device,
                                                const VkDeviceQueueInfo2* pQueueInfo,
                                                VkQueue* pQueue) {
    mImpl->on_vkGetDeviceQueue2(pool, device, pQueueInfo, pQueue);
}

void VkDecoderGlobalState::on_vkDestroyDevice(android::base::BumpPool* pool, VkDevice device,
                                              const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyDevice(pool, device, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkCreateBuffer(android::base::BumpPool* pool, VkDevice device,
                                                 const VkBufferCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkBuffer* pBuffer) {
    return mImpl->on_vkCreateBuffer(pool, device, pCreateInfo, pAllocator, pBuffer);
}

void VkDecoderGlobalState::on_vkDestroyBuffer(android::base::BumpPool* pool, VkDevice device,
                                              VkBuffer buffer,
                                              const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyBuffer(pool, device, buffer, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkBindBufferMemory(android::base::BumpPool* pool, VkDevice device,
                                                     VkBuffer buffer, VkDeviceMemory memory,
                                                     VkDeviceSize memoryOffset) {
    return mImpl->on_vkBindBufferMemory(pool, device, buffer, memory, memoryOffset);
}

VkResult VkDecoderGlobalState::on_vkBindBufferMemory2(android::base::BumpPool* pool,
                                                      VkDevice device, uint32_t bindInfoCount,
                                                      const VkBindBufferMemoryInfo* pBindInfos) {
    return mImpl->on_vkBindBufferMemory2(pool, device, bindInfoCount, pBindInfos);
}

VkResult VkDecoderGlobalState::on_vkBindBufferMemory2KHR(android::base::BumpPool* pool,
                                                         VkDevice device, uint32_t bindInfoCount,
                                                         const VkBindBufferMemoryInfo* pBindInfos) {
    return mImpl->on_vkBindBufferMemory2KHR(pool, device, bindInfoCount, pBindInfos);
}

VkResult VkDecoderGlobalState::on_vkCreateImage(android::base::BumpPool* pool, VkDevice device,
                                                const VkImageCreateInfo* pCreateInfo,
                                                const VkAllocationCallbacks* pAllocator,
                                                VkImage* pImage) {
    return mImpl->on_vkCreateImage(pool, device, pCreateInfo, pAllocator, pImage);
}

void VkDecoderGlobalState::on_vkDestroyImage(android::base::BumpPool* pool, VkDevice device,
                                             VkImage image,
                                             const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyImage(pool, device, image, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkBindImageMemory(android::base::BumpPool* pool, VkDevice device,
                                                    VkImage image, VkDeviceMemory memory,
                                                    VkDeviceSize memoryOffset) {
    return mImpl->on_vkBindImageMemory(pool, device, image, memory, memoryOffset);
}

VkResult VkDecoderGlobalState::on_vkBindImageMemory2(android::base::BumpPool* pool, VkDevice device,
                                                     uint32_t bindInfoCount,
                                                     const VkBindImageMemoryInfo* pBindInfos) {
    return mImpl->on_vkBindImageMemory2(pool, device, bindInfoCount, pBindInfos);
}

VkResult VkDecoderGlobalState::on_vkBindImageMemory2KHR(android::base::BumpPool* pool,
                                                        VkDevice device, uint32_t bindInfoCount,
                                                        const VkBindImageMemoryInfo* pBindInfos) {
    return mImpl->on_vkBindImageMemory2(pool, device, bindInfoCount, pBindInfos);
}

VkResult VkDecoderGlobalState::on_vkCreateImageView(android::base::BumpPool* pool, VkDevice device,
                                                    const VkImageViewCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkImageView* pView) {
    return mImpl->on_vkCreateImageView(pool, device, pCreateInfo, pAllocator, pView);
}

void VkDecoderGlobalState::on_vkDestroyImageView(android::base::BumpPool* pool, VkDevice device,
                                                 VkImageView imageView,
                                                 const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyImageView(pool, device, imageView, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkCreateSampler(android::base::BumpPool* pool, VkDevice device,
                                                  const VkSamplerCreateInfo* pCreateInfo,
                                                  const VkAllocationCallbacks* pAllocator,
                                                  VkSampler* pSampler) {
    return mImpl->on_vkCreateSampler(pool, device, pCreateInfo, pAllocator, pSampler);
}

void VkDecoderGlobalState::on_vkDestroySampler(android::base::BumpPool* pool, VkDevice device,
                                               VkSampler sampler,
                                               const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroySampler(pool, device, sampler, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkCreateSemaphore(android::base::BumpPool* pool, VkDevice device,
                                                    const VkSemaphoreCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkSemaphore* pSemaphore) {
    return mImpl->on_vkCreateSemaphore(pool, device, pCreateInfo, pAllocator, pSemaphore);
}

VkResult VkDecoderGlobalState::on_vkImportSemaphoreFdKHR(
    android::base::BumpPool* pool, VkDevice device,
    const VkImportSemaphoreFdInfoKHR* pImportSemaphoreFdInfo) {
    return mImpl->on_vkImportSemaphoreFdKHR(pool, device, pImportSemaphoreFdInfo);
}

VkResult VkDecoderGlobalState::on_vkGetSemaphoreFdKHR(android::base::BumpPool* pool,
                                                      VkDevice device,
                                                      const VkSemaphoreGetFdInfoKHR* pGetFdInfo,
                                                      int* pFd) {
    return mImpl->on_vkGetSemaphoreFdKHR(pool, device, pGetFdInfo, pFd);
}

VkResult VkDecoderGlobalState::on_vkGetSemaphoreGOOGLE(android::base::BumpPool* pool,
                                                       VkDevice device, VkSemaphore semaphore,
                                                       uint64_t syncId) {
    return mImpl->on_vkGetSemaphoreGOOGLE(pool, device, semaphore, syncId);
}

void VkDecoderGlobalState::on_vkDestroySemaphore(android::base::BumpPool* pool, VkDevice device,
                                                 VkSemaphore semaphore,
                                                 const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroySemaphore(pool, device, semaphore, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkCreateFence(android::base::BumpPool* pool, VkDevice device,
                                                const VkFenceCreateInfo* pCreateInfo,
                                                const VkAllocationCallbacks* pAllocator,
                                                VkFence* pFence) {
    return mImpl->on_vkCreateFence(pool, device, pCreateInfo, pAllocator, pFence);
}

VkResult VkDecoderGlobalState::on_vkResetFences(android::base::BumpPool* pool, VkDevice device,
                                                uint32_t fenceCount, const VkFence* pFences) {
    return mImpl->on_vkResetFences(pool, device, fenceCount, pFences);
}

void VkDecoderGlobalState::on_vkDestroyFence(android::base::BumpPool* pool, VkDevice device,
                                             VkFence fence,
                                             const VkAllocationCallbacks* pAllocator) {
    return mImpl->on_vkDestroyFence(pool, device, fence, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkCreateDescriptorSetLayout(
    android::base::BumpPool* pool, VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout) {
    return mImpl->on_vkCreateDescriptorSetLayout(pool, device, pCreateInfo, pAllocator, pSetLayout);
}

void VkDecoderGlobalState::on_vkDestroyDescriptorSetLayout(
    android::base::BumpPool* pool, VkDevice device, VkDescriptorSetLayout descriptorSetLayout,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyDescriptorSetLayout(pool, device, descriptorSetLayout, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkCreateDescriptorPool(
    android::base::BumpPool* pool, VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool) {
    return mImpl->on_vkCreateDescriptorPool(pool, device, pCreateInfo, pAllocator, pDescriptorPool);
}

void VkDecoderGlobalState::on_vkDestroyDescriptorPool(android::base::BumpPool* pool,
                                                      VkDevice device,
                                                      VkDescriptorPool descriptorPool,
                                                      const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyDescriptorPool(pool, device, descriptorPool, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkResetDescriptorPool(android::base::BumpPool* pool,
                                                        VkDevice device,
                                                        VkDescriptorPool descriptorPool,
                                                        VkDescriptorPoolResetFlags flags) {
    return mImpl->on_vkResetDescriptorPool(pool, device, descriptorPool, flags);
}

VkResult VkDecoderGlobalState::on_vkAllocateDescriptorSets(
    android::base::BumpPool* pool, VkDevice device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo, VkDescriptorSet* pDescriptorSets) {
    return mImpl->on_vkAllocateDescriptorSets(pool, device, pAllocateInfo, pDescriptorSets);
}

VkResult VkDecoderGlobalState::on_vkFreeDescriptorSets(android::base::BumpPool* pool,
                                                       VkDevice device,
                                                       VkDescriptorPool descriptorPool,
                                                       uint32_t descriptorSetCount,
                                                       const VkDescriptorSet* pDescriptorSets) {
    return mImpl->on_vkFreeDescriptorSets(pool, device, descriptorPool, descriptorSetCount,
                                          pDescriptorSets);
}

void VkDecoderGlobalState::on_vkUpdateDescriptorSets(android::base::BumpPool* pool, VkDevice device,
                                                     uint32_t descriptorWriteCount,
                                                     const VkWriteDescriptorSet* pDescriptorWrites,
                                                     uint32_t descriptorCopyCount,
                                                     const VkCopyDescriptorSet* pDescriptorCopies) {
    mImpl->on_vkUpdateDescriptorSets(pool, device, descriptorWriteCount, pDescriptorWrites,
                                     descriptorCopyCount, pDescriptorCopies);
}

VkResult VkDecoderGlobalState::on_vkCreateShaderModule(android::base::BumpPool* pool,
                                                       VkDevice boxed_device,
                                                       const VkShaderModuleCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkShaderModule* pShaderModule) {
    return mImpl->on_vkCreateShaderModule(pool, boxed_device, pCreateInfo, pAllocator,
                                          pShaderModule);
}

void VkDecoderGlobalState::on_vkDestroyShaderModule(android::base::BumpPool* pool,
                                                    VkDevice boxed_device,
                                                    VkShaderModule shaderModule,
                                                    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyShaderModule(pool, boxed_device, shaderModule, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkCreatePipelineCache(
    android::base::BumpPool* pool, VkDevice boxed_device,
    const VkPipelineCacheCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
    VkPipelineCache* pPipelineCache) {
    return mImpl->on_vkCreatePipelineCache(pool, boxed_device, pCreateInfo, pAllocator,
                                           pPipelineCache);
}

void VkDecoderGlobalState::on_vkDestroyPipelineCache(android::base::BumpPool* pool,
                                                     VkDevice boxed_device,
                                                     VkPipelineCache pipelineCache,
                                                     const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyPipelineCache(pool, boxed_device, pipelineCache, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkCreateGraphicsPipelines(
    android::base::BumpPool* pool, VkDevice boxed_device, VkPipelineCache pipelineCache,
    uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    return mImpl->on_vkCreateGraphicsPipelines(pool, boxed_device, pipelineCache, createInfoCount,
                                               pCreateInfos, pAllocator, pPipelines);
}

void VkDecoderGlobalState::on_vkDestroyPipeline(android::base::BumpPool* pool,
                                                VkDevice boxed_device, VkPipeline pipeline,
                                                const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyPipeline(pool, boxed_device, pipeline, pAllocator);
}

void VkDecoderGlobalState::on_vkCmdCopyBufferToImage(
    android::base::BumpPool* pool, VkCommandBuffer commandBuffer, VkBuffer srcBuffer,
    VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount,
    const VkBufferImageCopy* pRegions, const VkDecoderContext& context) {
    mImpl->on_vkCmdCopyBufferToImage(pool, commandBuffer, srcBuffer, dstImage, dstImageLayout,
                                     regionCount, pRegions, context);
}

void VkDecoderGlobalState::on_vkCmdCopyImage(android::base::BumpPool* pool,
                                             VkCommandBuffer commandBuffer, VkImage srcImage,
                                             VkImageLayout srcImageLayout, VkImage dstImage,
                                             VkImageLayout dstImageLayout, uint32_t regionCount,
                                             const VkImageCopy* pRegions) {
    mImpl->on_vkCmdCopyImage(pool, commandBuffer, srcImage, srcImageLayout, dstImage,
                             dstImageLayout, regionCount, pRegions);
}
void VkDecoderGlobalState::on_vkCmdCopyImageToBuffer(android::base::BumpPool* pool,
                                                     VkCommandBuffer commandBuffer,
                                                     VkImage srcImage, VkImageLayout srcImageLayout,
                                                     VkBuffer dstBuffer, uint32_t regionCount,
                                                     const VkBufferImageCopy* pRegions) {
    mImpl->on_vkCmdCopyImageToBuffer(pool, commandBuffer, srcImage, srcImageLayout, dstBuffer,
                                     regionCount, pRegions);
}

void VkDecoderGlobalState::on_vkCmdCopyBufferToImage2(android::base::BumpPool* pool,
                                VkCommandBuffer commandBuffer,
                                const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo,
                                const VkDecoderContext& context) {
    mImpl->on_vkCmdCopyBufferToImage2(pool, commandBuffer, pCopyBufferToImageInfo, context);
}

void VkDecoderGlobalState::on_vkCmdCopyImage2(android::base::BumpPool* pool,
    VkCommandBuffer commandBuffer,
    const VkCopyImageInfo2* pCopyImageInfo) {
    mImpl->on_vkCmdCopyImage2(pool, commandBuffer, pCopyImageInfo);
}

void VkDecoderGlobalState::on_vkCmdCopyImageToBuffer2(android::base::BumpPool* pool,
                                VkCommandBuffer commandBuffer,
                                const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo) {
    mImpl->on_vkCmdCopyImageToBuffer2(pool, commandBuffer, pCopyImageToBufferInfo);
}

void VkDecoderGlobalState::on_vkCmdCopyBufferToImage2KHR(android::base::BumpPool* pool,
                                VkCommandBuffer commandBuffer,
                                const VkCopyBufferToImageInfo2KHR* pCopyBufferToImageInfo,
                                const VkDecoderContext& context) {
    mImpl->on_vkCmdCopyBufferToImage2KHR(pool, commandBuffer, pCopyBufferToImageInfo, context);
}

void VkDecoderGlobalState::on_vkCmdCopyImage2KHR(android::base::BumpPool* pool,
    VkCommandBuffer commandBuffer,
    const VkCopyImageInfo2KHR* pCopyImageInfo) {
    mImpl->on_vkCmdCopyImage2KHR(pool, commandBuffer, pCopyImageInfo);
}

void VkDecoderGlobalState::on_vkCmdCopyImageToBuffer2KHR(android::base::BumpPool* pool,
                                VkCommandBuffer commandBuffer,
                                const VkCopyImageToBufferInfo2KHR* pCopyImageToBufferInfo) {
    mImpl->on_vkCmdCopyImageToBuffer2KHR(pool, commandBuffer, pCopyImageToBufferInfo);
}

void VkDecoderGlobalState::on_vkGetImageMemoryRequirements(
    android::base::BumpPool* pool, VkDevice device, VkImage image,
    VkMemoryRequirements* pMemoryRequirements) {
    mImpl->on_vkGetImageMemoryRequirements(pool, device, image, pMemoryRequirements);
}

void VkDecoderGlobalState::on_vkGetImageMemoryRequirements2(
    android::base::BumpPool* pool, VkDevice device, const VkImageMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements) {
    mImpl->on_vkGetImageMemoryRequirements2(pool, device, pInfo, pMemoryRequirements);
}

void VkDecoderGlobalState::on_vkGetImageMemoryRequirements2KHR(
    android::base::BumpPool* pool, VkDevice device, const VkImageMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements) {
    mImpl->on_vkGetImageMemoryRequirements2(pool, device, pInfo, pMemoryRequirements);
}

void VkDecoderGlobalState::on_vkGetBufferMemoryRequirements(
    android::base::BumpPool* pool, VkDevice device, VkBuffer buffer,
    VkMemoryRequirements* pMemoryRequirements) {
    mImpl->on_vkGetBufferMemoryRequirements(pool, device, buffer, pMemoryRequirements);
}

void VkDecoderGlobalState::on_vkGetBufferMemoryRequirements2(
    android::base::BumpPool* pool, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements) {
    mImpl->on_vkGetBufferMemoryRequirements2(pool, device, pInfo, pMemoryRequirements);
}

void VkDecoderGlobalState::on_vkGetBufferMemoryRequirements2KHR(
    android::base::BumpPool* pool, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements) {
    mImpl->on_vkGetBufferMemoryRequirements2(pool, device, pInfo, pMemoryRequirements);
}

void VkDecoderGlobalState::on_vkCmdPipelineBarrier(
    android::base::BumpPool* pool, VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers) {
    mImpl->on_vkCmdPipelineBarrier(pool, commandBuffer, srcStageMask, dstStageMask, dependencyFlags,
                                   memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount,
                                   pBufferMemoryBarriers, imageMemoryBarrierCount,
                                   pImageMemoryBarriers);
}

void VkDecoderGlobalState::on_vkCmdPipelineBarrier2(android::base::BumpPool* pool,
                                                    VkCommandBuffer commandBuffer,
                                                    const VkDependencyInfo* pDependencyInfo) {
    mImpl->on_vkCmdPipelineBarrier2(pool, commandBuffer, pDependencyInfo);
}

VkResult VkDecoderGlobalState::on_vkAllocateMemory(android::base::BumpPool* pool, VkDevice device,
                                                   const VkMemoryAllocateInfo* pAllocateInfo,
                                                   const VkAllocationCallbacks* pAllocator,
                                                   VkDeviceMemory* pMemory) {
    return mImpl->on_vkAllocateMemory(pool, device, pAllocateInfo, pAllocator, pMemory);
}

void VkDecoderGlobalState::on_vkFreeMemory(android::base::BumpPool* pool, VkDevice device,
                                           VkDeviceMemory memory,
                                           const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkFreeMemory(pool, device, memory, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkMapMemory(android::base::BumpPool* pool, VkDevice device,
                                              VkDeviceMemory memory, VkDeviceSize offset,
                                              VkDeviceSize size, VkMemoryMapFlags flags,
                                              void** ppData) {
    return mImpl->on_vkMapMemory(pool, device, memory, offset, size, flags, ppData);
}

void VkDecoderGlobalState::on_vkUnmapMemory(android::base::BumpPool* pool, VkDevice device,
                                            VkDeviceMemory memory) {
    mImpl->on_vkUnmapMemory(pool, device, memory);
}

uint8_t* VkDecoderGlobalState::getMappedHostPointer(VkDeviceMemory memory) {
    return mImpl->getMappedHostPointer(memory);
}

VkDeviceSize VkDecoderGlobalState::getDeviceMemorySize(VkDeviceMemory memory) {
    return mImpl->getDeviceMemorySize(memory);
}

bool VkDecoderGlobalState::usingDirectMapping() const { return mImpl->usingDirectMapping(); }

VkDecoderGlobalState::HostFeatureSupport VkDecoderGlobalState::getHostFeatureSupport() const {
    return mImpl->getHostFeatureSupport();
}

// VK_ANDROID_native_buffer
VkResult VkDecoderGlobalState::on_vkGetSwapchainGrallocUsageANDROID(android::base::BumpPool* pool,
                                                                    VkDevice device,
                                                                    VkFormat format,
                                                                    VkImageUsageFlags imageUsage,
                                                                    int* grallocUsage) {
    return mImpl->on_vkGetSwapchainGrallocUsageANDROID(pool, device, format, imageUsage,
                                                       grallocUsage);
}

VkResult VkDecoderGlobalState::on_vkGetSwapchainGrallocUsage2ANDROID(
    android::base::BumpPool* pool, VkDevice device, VkFormat format, VkImageUsageFlags imageUsage,
    VkSwapchainImageUsageFlagsANDROID swapchainImageUsage, uint64_t* grallocConsumerUsage,
    uint64_t* grallocProducerUsage) {
    return mImpl->on_vkGetSwapchainGrallocUsage2ANDROID(pool, device, format, imageUsage,
                                                        swapchainImageUsage, grallocConsumerUsage,
                                                        grallocProducerUsage);
}

VkResult VkDecoderGlobalState::on_vkAcquireImageANDROID(android::base::BumpPool* pool,
                                                        VkDevice device, VkImage image,
                                                        int nativeFenceFd, VkSemaphore semaphore,
                                                        VkFence fence) {
    return mImpl->on_vkAcquireImageANDROID(pool, device, image, nativeFenceFd, semaphore, fence);
}

VkResult VkDecoderGlobalState::on_vkQueueSignalReleaseImageANDROID(
    android::base::BumpPool* pool, VkQueue queue, uint32_t waitSemaphoreCount,
    const VkSemaphore* pWaitSemaphores, VkImage image, int* pNativeFenceFd) {
    return mImpl->on_vkQueueSignalReleaseImageANDROID(pool, queue, waitSemaphoreCount,
                                                      pWaitSemaphores, image, pNativeFenceFd);
}

// VK_GOOGLE_gfxstream
VkResult VkDecoderGlobalState::on_vkMapMemoryIntoAddressSpaceGOOGLE(android::base::BumpPool* pool,
                                                                    VkDevice device,
                                                                    VkDeviceMemory memory,
                                                                    uint64_t* pAddress) {
    return mImpl->on_vkMapMemoryIntoAddressSpaceGOOGLE(pool, device, memory, pAddress);
}

VkResult VkDecoderGlobalState::on_vkGetMemoryHostAddressInfoGOOGLE(
    android::base::BumpPool* pool, VkDevice device, VkDeviceMemory memory, uint64_t* pAddress,
    uint64_t* pSize, uint64_t* pHostmemId) {
    return mImpl->on_vkGetMemoryHostAddressInfoGOOGLE(pool, device, memory, pAddress, pSize,
                                                      pHostmemId);
}

VkResult VkDecoderGlobalState::on_vkGetBlobGOOGLE(android::base::BumpPool* pool, VkDevice device,
                                                  VkDeviceMemory memory) {
    return mImpl->on_vkGetBlobGOOGLE(pool, device, memory);
}

VkResult VkDecoderGlobalState::on_vkFreeMemorySyncGOOGLE(android::base::BumpPool* pool,
                                                         VkDevice device, VkDeviceMemory memory,
                                                         const VkAllocationCallbacks* pAllocator) {
    return mImpl->on_vkFreeMemorySyncGOOGLE(pool, device, memory, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkAllocateCommandBuffers(
    android::base::BumpPool* pool, VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo, VkCommandBuffer* pCommandBuffers) {
    return mImpl->on_vkAllocateCommandBuffers(pool, device, pAllocateInfo, pCommandBuffers);
}

VkResult VkDecoderGlobalState::on_vkCreateCommandPool(android::base::BumpPool* pool,
                                                      VkDevice device,
                                                      const VkCommandPoolCreateInfo* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator,
                                                      VkCommandPool* pCommandPool) {
    return mImpl->on_vkCreateCommandPool(pool, device, pCreateInfo, pAllocator, pCommandPool);
}

void VkDecoderGlobalState::on_vkDestroyCommandPool(android::base::BumpPool* pool, VkDevice device,
                                                   VkCommandPool commandPool,
                                                   const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyCommandPool(pool, device, commandPool, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkResetCommandPool(android::base::BumpPool* pool, VkDevice device,
                                                     VkCommandPool commandPool,
                                                     VkCommandPoolResetFlags flags) {
    return mImpl->on_vkResetCommandPool(pool, device, commandPool, flags);
}

void VkDecoderGlobalState::on_vkCmdExecuteCommands(android::base::BumpPool* pool,
                                                   VkCommandBuffer commandBuffer,
                                                   uint32_t commandBufferCount,
                                                   const VkCommandBuffer* pCommandBuffers) {
    return mImpl->on_vkCmdExecuteCommands(pool, commandBuffer, commandBufferCount, pCommandBuffers);
}

VkResult VkDecoderGlobalState::on_vkQueueSubmit(android::base::BumpPool* pool, VkQueue queue,
                                                uint32_t submitCount, const VkSubmitInfo* pSubmits,
                                                VkFence fence) {
    return mImpl->on_vkQueueSubmit(pool, queue, submitCount, pSubmits, fence);
}

VkResult VkDecoderGlobalState::on_vkQueueSubmit2(android::base::BumpPool* pool, VkQueue queue,
                                                 uint32_t submitCount,
                                                 const VkSubmitInfo2* pSubmits, VkFence fence) {
    return mImpl->on_vkQueueSubmit(pool, queue, submitCount, pSubmits, fence);
}

VkResult VkDecoderGlobalState::on_vkQueueWaitIdle(android::base::BumpPool* pool, VkQueue queue) {
    return mImpl->on_vkQueueWaitIdle(pool, queue);
}

VkResult VkDecoderGlobalState::on_vkResetCommandBuffer(android::base::BumpPool* pool,
                                                       VkCommandBuffer commandBuffer,
                                                       VkCommandBufferResetFlags flags) {
    return mImpl->on_vkResetCommandBuffer(pool, commandBuffer, flags);
}

void VkDecoderGlobalState::on_vkFreeCommandBuffers(android::base::BumpPool* pool, VkDevice device,
                                                   VkCommandPool commandPool,
                                                   uint32_t commandBufferCount,
                                                   const VkCommandBuffer* pCommandBuffers) {
    return mImpl->on_vkFreeCommandBuffers(pool, device, commandPool, commandBufferCount,
                                          pCommandBuffers);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceExternalSemaphoreProperties(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo,
    VkExternalSemaphoreProperties* pExternalSemaphoreProperties) {
    return mImpl->on_vkGetPhysicalDeviceExternalSemaphoreProperties(
        pool, physicalDevice, pExternalSemaphoreInfo, pExternalSemaphoreProperties);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(
    android::base::BumpPool* pool, VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo,
    VkExternalSemaphoreProperties* pExternalSemaphoreProperties) {
    return mImpl->on_vkGetPhysicalDeviceExternalSemaphoreProperties(
        pool, physicalDevice, pExternalSemaphoreInfo, pExternalSemaphoreProperties);
}

// Descriptor update templates
VkResult VkDecoderGlobalState::on_vkCreateDescriptorUpdateTemplate(
    android::base::BumpPool* pool, VkDevice boxed_device,
    const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
    return mImpl->on_vkCreateDescriptorUpdateTemplate(pool, boxed_device, pCreateInfo, pAllocator,
                                                      pDescriptorUpdateTemplate);
}

VkResult VkDecoderGlobalState::on_vkCreateDescriptorUpdateTemplateKHR(
    android::base::BumpPool* pool, VkDevice boxed_device,
    const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
    return mImpl->on_vkCreateDescriptorUpdateTemplateKHR(pool, boxed_device, pCreateInfo,
                                                         pAllocator, pDescriptorUpdateTemplate);
}

void VkDecoderGlobalState::on_vkDestroyDescriptorUpdateTemplate(
    android::base::BumpPool* pool, VkDevice boxed_device,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate, const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyDescriptorUpdateTemplate(pool, boxed_device, descriptorUpdateTemplate,
                                                pAllocator);
}

void VkDecoderGlobalState::on_vkDestroyDescriptorUpdateTemplateKHR(
    android::base::BumpPool* pool, VkDevice boxed_device,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate, const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyDescriptorUpdateTemplateKHR(pool, boxed_device, descriptorUpdateTemplate,
                                                   pAllocator);
}

void VkDecoderGlobalState::on_vkUpdateDescriptorSetWithTemplateSizedGOOGLE(
    android::base::BumpPool* pool, VkDevice boxed_device, VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate, uint32_t imageInfoCount,
    uint32_t bufferInfoCount, uint32_t bufferViewCount, const uint32_t* pImageInfoEntryIndices,
    const uint32_t* pBufferInfoEntryIndices, const uint32_t* pBufferViewEntryIndices,
    const VkDescriptorImageInfo* pImageInfos, const VkDescriptorBufferInfo* pBufferInfos,
    const VkBufferView* pBufferViews) {
    mImpl->on_vkUpdateDescriptorSetWithTemplateSizedGOOGLE(
        pool, boxed_device, descriptorSet, descriptorUpdateTemplate, imageInfoCount,
        bufferInfoCount, bufferViewCount, pImageInfoEntryIndices, pBufferInfoEntryIndices,
        pBufferViewEntryIndices, pImageInfos, pBufferInfos, pBufferViews);
}

void VkDecoderGlobalState::on_vkUpdateDescriptorSetWithTemplateSized2GOOGLE(
    android::base::BumpPool* pool, VkDevice boxed_device, VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate, uint32_t imageInfoCount,
    uint32_t bufferInfoCount, uint32_t bufferViewCount, uint32_t inlineUniformBlockCount,
    const uint32_t* pImageInfoEntryIndices, const uint32_t* pBufferInfoEntryIndices,
    const uint32_t* pBufferViewEntryIndices, const VkDescriptorImageInfo* pImageInfos,
    const VkDescriptorBufferInfo* pBufferInfos, const VkBufferView* pBufferViews,
    const uint8_t* pInlineUniformBlockData) {
    mImpl->on_vkUpdateDescriptorSetWithTemplateSized2GOOGLE(
        pool, boxed_device, descriptorSet, descriptorUpdateTemplate, imageInfoCount,
        bufferInfoCount, bufferViewCount, inlineUniformBlockCount, pImageInfoEntryIndices,
        pBufferInfoEntryIndices, pBufferViewEntryIndices, pImageInfos, pBufferInfos, pBufferViews,
        pInlineUniformBlockData);
}

VkResult VkDecoderGlobalState::on_vkBeginCommandBuffer(android::base::BumpPool* pool,
                                                       VkCommandBuffer commandBuffer,
                                                       const VkCommandBufferBeginInfo* pBeginInfo,
                                                       const VkDecoderContext& context) {
    return mImpl->on_vkBeginCommandBuffer(pool, commandBuffer, pBeginInfo, context);
}

void VkDecoderGlobalState::on_vkBeginCommandBufferAsyncGOOGLE(
    android::base::BumpPool* pool, VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo* pBeginInfo, const VkDecoderContext& context) {
    mImpl->on_vkBeginCommandBuffer(pool, commandBuffer, pBeginInfo, context);
}

VkResult VkDecoderGlobalState::on_vkEndCommandBuffer(android::base::BumpPool* pool,
                                                     VkCommandBuffer commandBuffer,
                                                     const VkDecoderContext& context) {
    return mImpl->on_vkEndCommandBuffer(pool, commandBuffer, context);
}

void VkDecoderGlobalState::on_vkEndCommandBufferAsyncGOOGLE(android::base::BumpPool* pool,
                                                            VkCommandBuffer commandBuffer,
                                                            const VkDecoderContext& context) {
    mImpl->on_vkEndCommandBufferAsyncGOOGLE(pool, commandBuffer, context);
}

void VkDecoderGlobalState::on_vkResetCommandBufferAsyncGOOGLE(android::base::BumpPool* pool,
                                                              VkCommandBuffer commandBuffer,
                                                              VkCommandBufferResetFlags flags) {
    mImpl->on_vkResetCommandBufferAsyncGOOGLE(pool, commandBuffer, flags);
}

void VkDecoderGlobalState::on_vkCommandBufferHostSyncGOOGLE(android::base::BumpPool* pool,
                                                            VkCommandBuffer commandBuffer,
                                                            uint32_t needHostSync,
                                                            uint32_t sequenceNumber) {
    mImpl->hostSyncCommandBuffer("hostSync", commandBuffer, needHostSync, sequenceNumber);
}

VkResult VkDecoderGlobalState::on_vkCreateImageWithRequirementsGOOGLE(
    android::base::BumpPool* pool, VkDevice device, const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkImage* pImage,
    VkMemoryRequirements* pMemoryRequirements) {
    return mImpl->on_vkCreateImageWithRequirementsGOOGLE(pool, device, pCreateInfo, pAllocator,
                                                         pImage, pMemoryRequirements);
}

VkResult VkDecoderGlobalState::on_vkCreateBufferWithRequirementsGOOGLE(
    android::base::BumpPool* pool, VkDevice device, const VkBufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer,
    VkMemoryRequirements* pMemoryRequirements) {
    return mImpl->on_vkCreateBufferWithRequirementsGOOGLE(pool, device, pCreateInfo, pAllocator,
                                                          pBuffer, pMemoryRequirements);
}

void VkDecoderGlobalState::on_vkCmdBindPipeline(android::base::BumpPool* pool,
                                                VkCommandBuffer commandBuffer,
                                                VkPipelineBindPoint pipelineBindPoint,
                                                VkPipeline pipeline) {
    mImpl->on_vkCmdBindPipeline(pool, commandBuffer, pipelineBindPoint, pipeline);
}

void VkDecoderGlobalState::on_vkCmdBindDescriptorSets(
    android::base::BumpPool* pool, VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet,
    uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets,
    uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets) {
    mImpl->on_vkCmdBindDescriptorSets(pool, commandBuffer, pipelineBindPoint, layout, firstSet,
                                      descriptorSetCount, pDescriptorSets, dynamicOffsetCount,
                                      pDynamicOffsets);
}

VkResult VkDecoderGlobalState::on_vkCreateRenderPass(android::base::BumpPool* pool,
                                                     VkDevice boxed_device,
                                                     const VkRenderPassCreateInfo* pCreateInfo,
                                                     const VkAllocationCallbacks* pAllocator,
                                                     VkRenderPass* pRenderPass) {
    return mImpl->on_vkCreateRenderPass(pool, boxed_device, pCreateInfo, pAllocator, pRenderPass);
}

VkResult VkDecoderGlobalState::on_vkCreateRenderPass2(android::base::BumpPool* pool,
                                                      VkDevice boxed_device,
                                                      const VkRenderPassCreateInfo2* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator,
                                                      VkRenderPass* pRenderPass) {
    return mImpl->on_vkCreateRenderPass2(pool, boxed_device, pCreateInfo, pAllocator, pRenderPass);
}

VkResult VkDecoderGlobalState::on_vkCreateRenderPass2KHR(
    android::base::BumpPool* pool, VkDevice boxed_device,
    const VkRenderPassCreateInfo2KHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
    VkRenderPass* pRenderPass) {
    return mImpl->on_vkCreateRenderPass2(pool, boxed_device, pCreateInfo, pAllocator, pRenderPass);
}

void VkDecoderGlobalState::on_vkDestroyRenderPass(android::base::BumpPool* pool,
                                                  VkDevice boxed_device, VkRenderPass renderPass,
                                                  const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyRenderPass(pool, boxed_device, renderPass, pAllocator);
}

void VkDecoderGlobalState::on_vkCmdBeginRenderPass(android::base::BumpPool* pool,
                                                   VkCommandBuffer commandBuffer,
                                                   const VkRenderPassBeginInfo* pRenderPassBegin,
                                                   VkSubpassContents contents) {
    return mImpl->on_vkCmdBeginRenderPass(pool, commandBuffer, pRenderPassBegin, contents);
}

void VkDecoderGlobalState::on_vkCmdBeginRenderPass2(android::base::BumpPool* pool,
                                                    VkCommandBuffer commandBuffer,
                                                    const VkRenderPassBeginInfo* pRenderPassBegin,
                                                    const VkSubpassBeginInfo* pSubpassBeginInfo) {
    return mImpl->on_vkCmdBeginRenderPass2(pool, commandBuffer, pRenderPassBegin,
                                           pSubpassBeginInfo);
}

void VkDecoderGlobalState::on_vkCmdBeginRenderPass2KHR(
    android::base::BumpPool* pool, VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin, const VkSubpassBeginInfo* pSubpassBeginInfo) {
    return mImpl->on_vkCmdBeginRenderPass2(pool, commandBuffer, pRenderPassBegin,
                                           pSubpassBeginInfo);
}

VkResult VkDecoderGlobalState::on_vkCreateFramebuffer(android::base::BumpPool* pool,
                                                      VkDevice boxed_device,
                                                      const VkFramebufferCreateInfo* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator,
                                                      VkFramebuffer* pFramebuffer) {
    return mImpl->on_vkCreateFramebuffer(pool, boxed_device, pCreateInfo, pAllocator, pFramebuffer);
}

void VkDecoderGlobalState::on_vkDestroyFramebuffer(android::base::BumpPool* pool,
                                                   VkDevice boxed_device, VkFramebuffer framebuffer,
                                                   const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyFramebuffer(pool, boxed_device, framebuffer, pAllocator);
}

void VkDecoderGlobalState::on_vkQueueHostSyncGOOGLE(android::base::BumpPool* pool, VkQueue queue,
                                                    uint32_t needHostSync,
                                                    uint32_t sequenceNumber) {
    mImpl->hostSyncQueue("hostSyncQueue", queue, needHostSync, sequenceNumber);
}

void VkDecoderGlobalState::on_vkCmdCopyQueryPoolResults(android::base::BumpPool* pool,
                                                        VkCommandBuffer commandBuffer,
                                                        VkQueryPool queryPool, uint32_t firstQuery,
                                                        uint32_t queryCount, VkBuffer dstBuffer,
                                                        VkDeviceSize dstOffset, VkDeviceSize stride,
                                                        VkQueryResultFlags flags) {
    mImpl->on_vkCmdCopyQueryPoolResults(pool, commandBuffer, queryPool, firstQuery, queryCount,
                                        dstBuffer, dstOffset, stride, flags);
}

void VkDecoderGlobalState::on_vkQueueSubmitAsyncGOOGLE(android::base::BumpPool* pool, VkQueue queue,
                                                       uint32_t submitCount,
                                                       const VkSubmitInfo* pSubmits,
                                                       VkFence fence) {
    mImpl->on_vkQueueSubmit(pool, queue, submitCount, pSubmits, fence);
}

void VkDecoderGlobalState::on_vkQueueSubmitAsync2GOOGLE(android::base::BumpPool* pool,
                                                        VkQueue queue, uint32_t submitCount,
                                                        const VkSubmitInfo2* pSubmits,
                                                        VkFence fence) {
    mImpl->on_vkQueueSubmit(pool, queue, submitCount, pSubmits, fence);
}

void VkDecoderGlobalState::on_vkQueueWaitIdleAsyncGOOGLE(android::base::BumpPool* pool,
                                                         VkQueue queue) {
    mImpl->on_vkQueueWaitIdle(pool, queue);
}

void VkDecoderGlobalState::on_vkQueueBindSparseAsyncGOOGLE(android::base::BumpPool* pool,
                                                           VkQueue queue, uint32_t bindInfoCount,
                                                           const VkBindSparseInfo* pBindInfo,
                                                           VkFence fence) {
    mImpl->on_vkQueueBindSparse(pool, queue, bindInfoCount, pBindInfo, fence);
}

void VkDecoderGlobalState::on_vkGetLinearImageLayoutGOOGLE(android::base::BumpPool* pool,
                                                           VkDevice device, VkFormat format,
                                                           VkDeviceSize* pOffset,
                                                           VkDeviceSize* pRowPitchAlignment) {
    mImpl->on_vkGetLinearImageLayoutGOOGLE(pool, device, format, pOffset, pRowPitchAlignment);
}

void VkDecoderGlobalState::on_vkGetLinearImageLayout2GOOGLE(android::base::BumpPool* pool,
                                                            VkDevice device,
                                                            const VkImageCreateInfo* pCreateInfo,
                                                            VkDeviceSize* pOffset,
                                                            VkDeviceSize* pRowPitchAlignment) {
    mImpl->on_vkGetLinearImageLayout2GOOGLE(pool, device, pCreateInfo, pOffset, pRowPitchAlignment);
}

void VkDecoderGlobalState::on_vkQueueFlushCommandsGOOGLE(android::base::BumpPool* pool,
                                                         VkQueue queue,
                                                         VkCommandBuffer commandBuffer,
                                                         VkDeviceSize dataSize, const void* pData,
                                                         const VkDecoderContext& context) {
    mImpl->on_vkQueueFlushCommandsGOOGLE(pool, queue, commandBuffer, dataSize, pData, context);
}

void VkDecoderGlobalState::on_vkQueueFlushCommandsFromAuxMemoryGOOGLE(
    android::base::BumpPool* pool, VkQueue queue, VkCommandBuffer commandBuffer,
    VkDeviceMemory deviceMemory, VkDeviceSize dataOffset, VkDeviceSize dataSize,
    const VkDecoderContext& context) {
    mImpl->on_vkQueueFlushCommandsFromAuxMemoryGOOGLE(pool, queue, commandBuffer, deviceMemory,
                                                      dataOffset, dataSize, context);
}

void VkDecoderGlobalState::on_vkQueueCommitDescriptorSetUpdatesGOOGLE(
    android::base::BumpPool* pool, VkQueue queue, uint32_t descriptorPoolCount,
    const VkDescriptorPool* pDescriptorPools, uint32_t descriptorSetCount,
    const VkDescriptorSetLayout* pDescriptorSetLayouts, const uint64_t* pDescriptorSetPoolIds,
    const uint32_t* pDescriptorSetWhichPool, const uint32_t* pDescriptorSetPendingAllocation,
    const uint32_t* pDescriptorWriteStartingIndices, uint32_t pendingDescriptorWriteCount,
    const VkWriteDescriptorSet* pPendingDescriptorWrites) {
    mImpl->on_vkQueueCommitDescriptorSetUpdatesGOOGLE(
        pool, queue, descriptorPoolCount, pDescriptorPools, descriptorSetCount,
        pDescriptorSetLayouts, pDescriptorSetPoolIds, pDescriptorSetWhichPool,
        pDescriptorSetPendingAllocation, pDescriptorWriteStartingIndices,
        pendingDescriptorWriteCount, pPendingDescriptorWrites);
}

void VkDecoderGlobalState::on_vkCollectDescriptorPoolIdsGOOGLE(android::base::BumpPool* pool,
                                                               VkDevice device,
                                                               VkDescriptorPool descriptorPool,
                                                               uint32_t* pPoolIdCount,
                                                               uint64_t* pPoolIds) {
    mImpl->on_vkCollectDescriptorPoolIdsGOOGLE(pool, device, descriptorPool, pPoolIdCount,
                                               pPoolIds);
}

VkResult VkDecoderGlobalState::on_vkQueueBindSparse(android::base::BumpPool* pool, VkQueue queue,
                                                    uint32_t bindInfoCount,
                                                    const VkBindSparseInfo* pBindInfo,
                                                    VkFence fence) {
    return mImpl->on_vkQueueBindSparse(pool, queue, bindInfoCount, pBindInfo, fence);
}

void VkDecoderGlobalState::on_vkQueueSignalReleaseImageANDROIDAsyncGOOGLE(
    android::base::BumpPool* pool, VkQueue queue, uint32_t waitSemaphoreCount,
    const VkSemaphore* pWaitSemaphores, VkImage image) {
    int fenceFd;
    mImpl->on_vkQueueSignalReleaseImageANDROID(pool, queue, waitSemaphoreCount, pWaitSemaphores,
                                               image, &fenceFd);
}

VkResult VkDecoderGlobalState::on_vkCreateSamplerYcbcrConversion(
    android::base::BumpPool* pool, VkDevice device,
    const VkSamplerYcbcrConversionCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
    VkSamplerYcbcrConversion* pYcbcrConversion) {
    return mImpl->on_vkCreateSamplerYcbcrConversion(pool, device, pCreateInfo, pAllocator,
                                                    pYcbcrConversion);
}

VkResult VkDecoderGlobalState::on_vkCreateSamplerYcbcrConversionKHR(
    android::base::BumpPool* pool, VkDevice device,
    const VkSamplerYcbcrConversionCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
    VkSamplerYcbcrConversion* pYcbcrConversion) {
    return mImpl->on_vkCreateSamplerYcbcrConversion(pool, device, pCreateInfo, pAllocator,
                                                    pYcbcrConversion);
}

void VkDecoderGlobalState::on_vkDestroySamplerYcbcrConversion(
    android::base::BumpPool* pool, VkDevice device, VkSamplerYcbcrConversion ycbcrConversion,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroySamplerYcbcrConversion(pool, device, ycbcrConversion, pAllocator);
}

void VkDecoderGlobalState::on_vkDestroySamplerYcbcrConversionKHR(
    android::base::BumpPool* pool, VkDevice device, VkSamplerYcbcrConversion ycbcrConversion,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroySamplerYcbcrConversion(pool, device, ycbcrConversion, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkEnumeratePhysicalDeviceGroups(
    android::base::BumpPool* pool, VkInstance instance, uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties) {
    return mImpl->on_vkEnumeratePhysicalDeviceGroups(pool, instance, pPhysicalDeviceGroupCount,
                                                     pPhysicalDeviceGroupProperties);
}

VkResult VkDecoderGlobalState::on_vkEnumeratePhysicalDeviceGroupsKHR(
    android::base::BumpPool* pool, VkInstance instance, uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties) {
    return mImpl->on_vkEnumeratePhysicalDeviceGroups(pool, instance, pPhysicalDeviceGroupCount,
                                                     pPhysicalDeviceGroupProperties);
}

void VkDecoderGlobalState::on_DeviceLost() { mImpl->on_DeviceLost(); }

void VkDecoderGlobalState::on_CheckOutOfMemory(VkResult result, uint32_t opCode,
                                               const VkDecoderContext& context,
                                               std::optional<uint64_t> allocationSize) {
    mImpl->on_CheckOutOfMemory(result, opCode, context, allocationSize);
}

VkResult VkDecoderGlobalState::waitForFence(VkFence boxed_fence, uint64_t timeout) {
    return mImpl->waitForFence(boxed_fence, timeout);
}

VkResult VkDecoderGlobalState::getFenceStatus(VkFence boxed_fence) {
    return mImpl->getFenceStatus(boxed_fence);
}

AsyncResult VkDecoderGlobalState::registerQsriCallback(VkImage image,
                                                       VkQsriTimeline::Callback callback) {
    return mImpl->registerQsriCallback(image, std::move(callback));
}

void VkDecoderGlobalState::deviceMemoryTransform_tohost(VkDeviceMemory* memory,
                                                        uint32_t memoryCount, VkDeviceSize* offset,
                                                        uint32_t offsetCount, VkDeviceSize* size,
                                                        uint32_t sizeCount, uint32_t* typeIndex,
                                                        uint32_t typeIndexCount, uint32_t* typeBits,
                                                        uint32_t typeBitsCount) {
    // Not used currently
    (void)memory;
    (void)memoryCount;
    (void)offset;
    (void)offsetCount;
    (void)size;
    (void)sizeCount;
    (void)typeIndex;
    (void)typeIndexCount;
    (void)typeBits;
    (void)typeBitsCount;
}

void VkDecoderGlobalState::deviceMemoryTransform_fromhost(
    VkDeviceMemory* memory, uint32_t memoryCount, VkDeviceSize* offset, uint32_t offsetCount,
    VkDeviceSize* size, uint32_t sizeCount, uint32_t* typeIndex, uint32_t typeIndexCount,
    uint32_t* typeBits, uint32_t typeBitsCount) {
    // Not used currently
    (void)memory;
    (void)memoryCount;
    (void)offset;
    (void)offsetCount;
    (void)size;
    (void)sizeCount;
    (void)typeIndex;
    (void)typeIndexCount;
    (void)typeBits;
    (void)typeBitsCount;
}

VkDecoderSnapshot* VkDecoderGlobalState::snapshot() { return mImpl->snapshot(); }

#define DEFINE_TRANSFORMED_TYPE_IMPL(type)                                                        \
    void VkDecoderGlobalState::transformImpl_##type##_tohost(const type* val, uint32_t count) {   \
        mImpl->transformImpl_##type##_tohost(val, count);                                         \
    }                                                                                             \
    void VkDecoderGlobalState::transformImpl_##type##_fromhost(const type* val, uint32_t count) { \
        mImpl->transformImpl_##type##_fromhost(val, count);                                       \
    }

LIST_TRANSFORMED_TYPES(DEFINE_TRANSFORMED_TYPE_IMPL)

#define DEFINE_BOXED_DISPATCHABLE_HANDLE_API_DEF(type)                                         \
    type VkDecoderGlobalState::new_boxed_##type(type underlying, VulkanDispatch* dispatch,     \
                                                bool ownDispatch) {                            \
        return mImpl->new_boxed_##type(underlying, dispatch, ownDispatch);                     \
    }                                                                                          \
    void VkDecoderGlobalState::delete_##type(type boxed) { mImpl->delete_##type(boxed); }      \
    type VkDecoderGlobalState::unbox_##type(type boxed) { return mImpl->unbox_##type(boxed); } \
    type VkDecoderGlobalState::unboxed_to_boxed_##type(type unboxed) {                         \
        return mImpl->unboxed_to_boxed_##type(unboxed);                                        \
    }                                                                                          \
    VulkanDispatch* VkDecoderGlobalState::dispatch_##type(type boxed) {                        \
        return mImpl->dispatch_##type(boxed);                                                  \
    }

#define DEFINE_BOXED_NON_DISPATCHABLE_HANDLE_API_DEF(type)                                     \
    type VkDecoderGlobalState::new_boxed_non_dispatchable_##type(type underlying) {            \
        return mImpl->new_boxed_non_dispatchable_##type(underlying);                           \
    }                                                                                          \
    void VkDecoderGlobalState::delete_##type(type boxed) { mImpl->delete_##type(boxed); }      \
    type VkDecoderGlobalState::unbox_##type(type boxed) { return mImpl->unbox_##type(boxed); } \
    type VkDecoderGlobalState::unboxed_to_boxed_non_dispatchable_##type(type unboxed) {        \
        return mImpl->unboxed_to_boxed_non_dispatchable_##type(unboxed);                       \
    }

GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(DEFINE_BOXED_DISPATCHABLE_HANDLE_API_DEF)
GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(DEFINE_BOXED_NON_DISPATCHABLE_HANDLE_API_DEF)

#define DEFINE_BOXED_DISPATCHABLE_HANDLE_GLOBAL_API_DEF(type)                                     \
    type unbox_##type(type boxed) {                                                               \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt) return VK_NULL_HANDLE;                                                          \
        return (type)elt->underlying;                                                             \
    }                                                                                             \
    VulkanDispatch* dispatch_##type(type boxed) {                                                 \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt) {                                                                               \
            fprintf(stderr, "%s: err not found boxed %p\n", __func__, boxed);                     \
            return nullptr;                                                                       \
        }                                                                                         \
        return elt->dispatch;                                                                     \
    }                                                                                             \
    void delete_##type(type boxed) {                                                              \
        if (!boxed) return;                                                                       \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt) return;                                                                         \
        releaseOrderMaintInfo(elt->ordMaintInfo);                                                 \
        if (elt->readStream) {                                                                    \
            sReadStreamRegistry.push(elt->readStream);                                            \
            elt->readStream = nullptr;                                                            \
        }                                                                                         \
        sBoxedHandleManager.remove((uint64_t)boxed);                                              \
    }                                                                                             \
    type unboxed_to_boxed_##type(type unboxed) {                                                  \
        AutoLock lock(sBoxedHandleManager.lock);                                                  \
        return (type)sBoxedHandleManager.getBoxedFromUnboxedLocked((uint64_t)(uintptr_t)unboxed); \
    }

#define DEFINE_BOXED_NON_DISPATCHABLE_HANDLE_GLOBAL_API_DEF(type)                                 \
    type new_boxed_non_dispatchable_##type(type underlying) {                                     \
        return VkDecoderGlobalState::get()->new_boxed_non_dispatchable_##type(underlying);        \
    }                                                                                             \
    void delete_##type(type boxed) {                                                              \
        if (!boxed) return;                                                                       \
        sBoxedHandleManager.remove((uint64_t)boxed);                                              \
    }                                                                                             \
    void delayed_delete_##type(type boxed, VkDevice device, std::function<void()> callback) {     \
        sBoxedHandleManager.removeDelayed((uint64_t)boxed, device, callback);                     \
    }                                                                                             \
    type unbox_##type(type boxed) {                                                               \
        if (!boxed) return boxed;                                                                 \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt) {                                                                               \
            GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))                                       \
                << "Unbox " << boxed << " failed, not found.";                                    \
            return VK_NULL_HANDLE;                                                                \
        }                                                                                         \
        return (type)elt->underlying;                                                             \
    }                                                                                             \
    type unboxed_to_boxed_non_dispatchable_##type(type unboxed) {                                 \
        if (!unboxed) {                                                                           \
            return nullptr;                                                                       \
        }                                                                                         \
        AutoLock lock(sBoxedHandleManager.lock);                                                  \
        return (type)sBoxedHandleManager.getBoxedFromUnboxedLocked((uint64_t)(uintptr_t)unboxed); \
    }

GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(DEFINE_BOXED_DISPATCHABLE_HANDLE_GLOBAL_API_DEF)
GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(DEFINE_BOXED_NON_DISPATCHABLE_HANDLE_GLOBAL_API_DEF)

void BoxedHandleUnwrapAndDeletePreserveBoxedMapping::setup(android::base::BumpPool* pool,
                                                           uint64_t** bufPtr) {
    mPool = pool;
    mPreserveBufPtr = bufPtr;
}

void BoxedHandleUnwrapAndDeletePreserveBoxedMapping::allocPreserve(size_t count) {
    *mPreserveBufPtr = (uint64_t*)mPool->alloc(count * sizeof(uint64_t));
}

#define BOXED_DISPATCHABLE_HANDLE_UNWRAP_AND_DELETE_PRESERVE_BOXED_IMPL(type_name)        \
    void BoxedHandleUnwrapAndDeletePreserveBoxedMapping::mapHandles_##type_name(          \
        type_name* handles, size_t count) {                                               \
        allocPreserve(count);                                                             \
        for (size_t i = 0; i < count; ++i) {                                              \
            (*mPreserveBufPtr)[i] = (uint64_t)(handles[i]);                               \
            if (handles[i]) {                                                             \
                handles[i] = VkDecoderGlobalState::get()->unbox_##type_name(handles[i]);  \
            } else {                                                                      \
                handles[i] = (type_name) nullptr;                                         \
            };                                                                            \
        }                                                                                 \
    }                                                                                     \
    void BoxedHandleUnwrapAndDeletePreserveBoxedMapping::mapHandles_##type_name##_u64(    \
        const type_name* handles, uint64_t* handle_u64s, size_t count) {                  \
        allocPreserve(count);                                                             \
        for (size_t i = 0; i < count; ++i) {                                              \
            (*mPreserveBufPtr)[i] = (uint64_t)(handle_u64s[i]);                           \
            if (handles[i]) {                                                             \
                handle_u64s[i] =                                                          \
                    (uint64_t)VkDecoderGlobalState::get()->unbox_##type_name(handles[i]); \
            } else {                                                                      \
                handle_u64s[i] = 0;                                                       \
            }                                                                             \
        }                                                                                 \
    }                                                                                     \
    void BoxedHandleUnwrapAndDeletePreserveBoxedMapping::mapHandles_u64_##type_name(      \
        const uint64_t* handle_u64s, type_name* handles, size_t count) {                  \
        allocPreserve(count);                                                             \
        for (size_t i = 0; i < count; ++i) {                                              \
            (*mPreserveBufPtr)[i] = (uint64_t)(handle_u64s[i]);                           \
            if (handle_u64s[i]) {                                                         \
                handles[i] = VkDecoderGlobalState::get()->unbox_##type_name(              \
                    (type_name)(uintptr_t)handle_u64s[i]);                                \
            } else {                                                                      \
                handles[i] = (type_name) nullptr;                                         \
            }                                                                             \
        }                                                                                 \
    }

#define BOXED_NON_DISPATCHABLE_HANDLE_UNWRAP_AND_DELETE_PRESERVE_BOXED_IMPL(type_name)    \
    void BoxedHandleUnwrapAndDeletePreserveBoxedMapping::mapHandles_##type_name(          \
        type_name* handles, size_t count) {                                               \
        allocPreserve(count);                                                             \
        for (size_t i = 0; i < count; ++i) {                                              \
            (*mPreserveBufPtr)[i] = (uint64_t)(handles[i]);                               \
            if (handles[i]) {                                                             \
                auto boxed = handles[i];                                                  \
                handles[i] = VkDecoderGlobalState::get()->unbox_##type_name(handles[i]);  \
                delete_##type_name(boxed);                                                \
            } else {                                                                      \
                handles[i] = (type_name) nullptr;                                         \
            };                                                                            \
        }                                                                                 \
    }                                                                                     \
    void BoxedHandleUnwrapAndDeletePreserveBoxedMapping::mapHandles_##type_name##_u64(    \
        const type_name* handles, uint64_t* handle_u64s, size_t count) {                  \
        allocPreserve(count);                                                             \
        for (size_t i = 0; i < count; ++i) {                                              \
            (*mPreserveBufPtr)[i] = (uint64_t)(handle_u64s[i]);                           \
            if (handles[i]) {                                                             \
                auto boxed = handles[i];                                                  \
                handle_u64s[i] =                                                          \
                    (uint64_t)VkDecoderGlobalState::get()->unbox_##type_name(handles[i]); \
                delete_##type_name(boxed);                                                \
            } else {                                                                      \
                handle_u64s[i] = 0;                                                       \
            }                                                                             \
        }                                                                                 \
    }                                                                                     \
    void BoxedHandleUnwrapAndDeletePreserveBoxedMapping::mapHandles_u64_##type_name(      \
        const uint64_t* handle_u64s, type_name* handles, size_t count) {                  \
        allocPreserve(count);                                                             \
        for (size_t i = 0; i < count; ++i) {                                              \
            (*mPreserveBufPtr)[i] = (uint64_t)(handle_u64s[i]);                           \
            if (handle_u64s[i]) {                                                         \
                auto boxed = (type_name)(uintptr_t)handle_u64s[i];                        \
                handles[i] = VkDecoderGlobalState::get()->unbox_##type_name(              \
                    (type_name)(uintptr_t)handle_u64s[i]);                                \
                delete_##type_name(boxed);                                                \
            } else {                                                                      \
                handles[i] = (type_name) nullptr;                                         \
            }                                                                             \
        }                                                                                 \
    }

GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(
    BOXED_DISPATCHABLE_HANDLE_UNWRAP_AND_DELETE_PRESERVE_BOXED_IMPL)
GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(
    BOXED_NON_DISPATCHABLE_HANDLE_UNWRAP_AND_DELETE_PRESERVE_BOXED_IMPL)

}  // namespace vk
}  // namespace gfxstream
