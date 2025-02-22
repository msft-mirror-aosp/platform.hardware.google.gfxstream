// Copyright (C) 2025 The Android Open Source Project
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

#include "VulkanBoxedHandles.h"

#include "VkDecoderGlobalState.h"
#include "VkDecoderInternalStructs.h"

namespace gfxstream {
namespace vk {
namespace {

struct ReadStreamRegistry {
    android::base::Lock mLock;

    std::vector<VulkanMemReadingStream*> freeStreams;

    ReadStreamRegistry() { freeStreams.reserve(100); };

    VulkanMemReadingStream* pop(const gfxstream::host::FeatureSet& features) {
        android::base::AutoLock lock(mLock);
        if (freeStreams.empty()) {
            return new VulkanMemReadingStream(nullptr, features);
        } else {
            VulkanMemReadingStream* res = freeStreams.back();
            freeStreams.pop_back();
            return res;
        }
    }

    void push(VulkanMemReadingStream* stream) {
        android::base::AutoLock lock(mLock);
        freeStreams.push_back(stream);
    }
};

static ReadStreamRegistry sReadStreamRegistry;

}  // namespace

BoxedHandleManager sBoxedHandleManager;

#define DEFINE_BOXED_DISPATCHABLE_HANDLE_GLOBAL_API_DEF(type)                                     \
    type new_boxed_##type(type underlying, VulkanDispatch* dispatch, bool ownDispatch) {          \
        BoxedHandleInfo item;                                                                     \
        item.underlying = (uint64_t)underlying;                                                   \
        item.dispatch = dispatch ? dispatch : new VulkanDispatch;                                 \
        item.ownDispatch = ownDispatch;                                                           \
        item.ordMaintInfo = new OrderMaintenanceInfo;                                             \
        item.readStream = nullptr;                                                                \
        auto res = (type)sBoxedHandleManager.add(item, Tag_##type);                               \
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
            stream = sReadStreamRegistry.pop(VkDecoderGlobalState::get()->getFeatures());         \
            elt->readStream = stream;                                                             \
        }                                                                                         \
        return stream;                                                                            \
    }                                                                                             \
    VulkanDispatch* dispatch_##type(type boxed) {                                                 \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt) {                                                                               \
            ERR("%s: Failed to unbox %p", __func__, boxed);                                       \
            return nullptr;                                                                       \
        }                                                                                         \
        return elt->dispatch;                                                                     \
    }

#define DEFINE_BOXED_NON_DISPATCHABLE_HANDLE_GLOBAL_API_DEF(type)                                 \
    type new_boxed_non_dispatchable_##type(type underlying) {                                     \
        BoxedHandleInfo item;                                                                     \
        item.underlying = (uint64_t)underlying;                                                   \
        return (type)sBoxedHandleManager.add(item, Tag_##type);                                   \
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
    type try_unbox_##type(type boxed) {                                                           \
        if (!boxed) return boxed;                                                                 \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt) {                                                                               \
            WARN("%s: Failed to unbox %p", __func__, boxed);                                      \
            return VK_NULL_HANDLE;                                                                \
        }                                                                                         \
        return (type)elt->underlying;                                                             \
    }                                                                                             \
    type unboxed_to_boxed_non_dispatchable_##type(type unboxed) {                                 \
        if (!unboxed) {                                                                           \
            return nullptr;                                                                       \
        }                                                                                         \
        android::base::AutoLock lock(sBoxedHandleManager.lock);                                   \
        return (type)sBoxedHandleManager.getBoxedFromUnboxedLocked((uint64_t)(uintptr_t)unboxed); \
    }                                                                                             \
    void set_boxed_non_dispatchable_##type(type boxed, type underlying) {                         \
        BoxedHandleInfo item;                                                                     \
        item.underlying = (uint64_t)underlying;                                                   \
        sBoxedHandleManager.update((uint64_t)boxed, item, Tag_##type);                            \
    }

GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(DEFINE_BOXED_DISPATCHABLE_HANDLE_GLOBAL_API_DEF)
GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(DEFINE_BOXED_NON_DISPATCHABLE_HANDLE_GLOBAL_API_DEF)

#define DEFINE_BOXED_DISPATCHABLE_HANDLE_API_REGULAR_UNBOX_IMPL(type)                             \
    type unbox_##type(type boxed) {                                                               \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt){                                                                                \
            ERR("%s: Failed to unbox %p", __func__, boxed);                                       \
            return VK_NULL_HANDLE;                                                                \
        }                                                                                         \
        return (type)elt->underlying;                                                             \
    }                                                                                             \
    type try_unbox_##type(type boxed) {                                                           \
        auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);                           \
        if (!elt){                                                                                \
            WARN("%s: Failed to unbox %p", __func__, boxed);                                      \
            return VK_NULL_HANDLE;                                                                \
        }                                                                                         \
        return (type)elt->underlying;                                                             \
    }                                                                                             \
    type unboxed_to_boxed_##type(type unboxed) {                                                  \
        android::base::AutoLock lock(sBoxedHandleManager.lock);                                   \
        return (type)sBoxedHandleManager.getBoxedFromUnboxedLocked((uint64_t)(uintptr_t)unboxed); \
    }

GOLDFISH_VK_LIST_DISPATCHABLE_REGULAR_UNBOX_HANDLE_TYPES(DEFINE_BOXED_DISPATCHABLE_HANDLE_API_REGULAR_UNBOX_IMPL)

// Custom unbox_* functions or GOLDFISH_VK_LIST_DISPATCHABLE_CUSTOM_UNBOX_HANDLE_TYPES
// VkQueue objects can be virtual, meaning that multiple boxed queues can map into a single
// physical queue on the host GPU. Some conversion is needed for unboxing to physical.
VkQueue unbox_VkQueueImp(VkQueue boxed) {
    auto elt = sBoxedHandleManager.get((uint64_t)(uintptr_t)boxed);
    if (!elt) {
        return VK_NULL_HANDLE;
    }
    const uint64_t unboxedQueue64 = elt->underlying;
    // Use VulkanVirtualQueue directly to avoid locking for hasVirtualGraphicsQueue call.
    if (VkDecoderGlobalState::get()->getFeatures().VulkanVirtualQueue.enabled) {
        // Clear virtual bit and unbox into the actual physical queue handle
        return (VkQueue)(unboxedQueue64 & ~QueueInfo::kVirtualQueueBit);
    }
    return (VkQueue)(unboxedQueue64);
}

VkQueue unbox_VkQueue(VkQueue boxed) {
    VkQueue unboxed = unbox_VkQueueImp(boxed);
    if (unboxed == VK_NULL_HANDLE) {
        ERR("%s: Failed to unbox %p", __func__, boxed);
    }
    return unboxed;
}

VkQueue try_unbox_VkQueue(VkQueue boxed) {
    VkQueue unboxed = unbox_VkQueueImp(boxed);
    if (unboxed == VK_NULL_HANDLE) {
        WARN("%s: Failed to unbox %p", __func__, boxed);
    }
    return unboxed;
}

}  // namespace vk
}  // namespace gfxstream