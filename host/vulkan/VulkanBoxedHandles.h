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

#pragma once

#include <deque>
#include <mutex>

#include "VulkanDispatch.h"
#include "VulkanHandles.h"
#include "VulkanStream.h"
#include "aemu/base/ThreadAnnotations.h"
#include "aemu/base/containers/HybridEntityManager.h"
#include "aemu/base/containers/Lookup.h"
#include "aemu/base/synchronization/ConditionVariable.h"
#include "aemu/base/synchronization/Lock.h"

namespace gfxstream {
namespace vk {

#define DEFINE_BOXED_HANDLE_TYPE_TAG(type) Tag_##type,

enum BoxedHandleTypeTag {
    Tag_Invalid = 0,

    GOLDFISH_VK_LIST_HANDLE_TYPES_BY_STAGE(DEFINE_BOXED_HANDLE_TYPE_TAG)

    // additional generic tag
    Tag_VkGeneric = 1001,
};

using BoxedHandle = uint64_t;
using UnboxedHandle = uint64_t;

struct OrderMaintenanceInfo {
    uint32_t sequenceNumber = 0;
    android::base::Lock lock;
    android::base::ConditionVariable cv;

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

class BoxedHandleInfo {
   public:
    UnboxedHandle underlying;
    VulkanDispatch* dispatch = nullptr;
    bool ownDispatch = false;
    OrderMaintenanceInfo* ordMaintInfo = nullptr;
    VulkanMemReadingStream* readStream = nullptr;
};

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
    using Store = android::base::HybridEntityManager<16000, BoxedHandle, BoxedHandleInfo>;

    void replayHandles(std::vector<BoxedHandle> handles) {
        mHandleReplay = true;
        mHandleReplayQueue.clear();
        for (BoxedHandle handle : handles) {
            mHandleReplayQueue.push_back(handle);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mMutex);
        mReverseMap.clear();
        mStore.clear();
    }

    BoxedHandle add(const BoxedHandleInfo& item, BoxedHandleTypeTag tag) {
        BoxedHandle handle;

        if (mHandleReplay) {
            handle = mHandleReplayQueue.front();
            mHandleReplayQueue.pop_front();
            mHandleReplay = !mHandleReplayQueue.empty();

            handle = (BoxedHandle)mStore.addFixed(handle, item, (size_t)tag);
        } else {
            handle = (BoxedHandle)mStore.add(item, (size_t)tag);
        }

        std::lock_guard<std::mutex> lock(mMutex);
        mReverseMap[(BoxedHandle)(item.underlying)] = handle;
        return handle;
    }

    void update(BoxedHandle handle, const BoxedHandleInfo& item, BoxedHandleTypeTag tag) {
        auto storedItem = mStore.get(handle);
        UnboxedHandle oldHandle = (UnboxedHandle)storedItem->underlying;
        *storedItem = item;
        std::lock_guard<std::mutex> lock(mMutex);
        if (oldHandle) {
            mReverseMap.erase(oldHandle);
        }
        mReverseMap[(UnboxedHandle)(item.underlying)] = handle;
    }

    void remove(BoxedHandle h) {
        auto item = get(h);
        if (item) {
            std::lock_guard<std::mutex> lock(mMutex);
            mReverseMap.erase((UnboxedHandle)(item->underlying));
        }
        mStore.remove(h);
    }

    void removeDelayed(uint64_t h, VkDevice device, std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(mMutex);
        mDelayedRemoves[device].push_back({h, callback});
    }

    // Do not call directly! Instead use `processDelayedRemovesForDevice()` which has
    // thread safety annotations for `VkDecoderGlobalState::Impl`.
    void processDelayedRemoves(VkDevice device) {
        std::vector<DelayedRemove> deviceDelayedRemoves;

        {
            std::lock_guard<std::mutex> lock(mMutex);

            auto it = mDelayedRemoves.find(device);
            if (it == mDelayedRemoves.end()) return;

            deviceDelayedRemoves = std::move(it->second);
            mDelayedRemoves.erase(it);
        }

        for (const auto& r : deviceDelayedRemoves) {
            auto h = r.handle;

            // VkDecoderGlobalState is not locked when callback is called.
            if (r.callback) {
                r.callback();
            }

            mStore.remove(h);
        }
    }

    BoxedHandleInfo* get(BoxedHandle handle) {
        return (BoxedHandleInfo*)mStore.get_const(handle);
    }

    BoxedHandle getBoxedFromUnboxed(UnboxedHandle unboxed) {
        std::lock_guard<std::mutex> lock(mMutex);

        auto it = mReverseMap.find(unboxed);
        if (it == mReverseMap.end()) {
            return 0;
        }

        return it->second;
    }

  private:
    mutable Store mStore;

    std::mutex mMutex;
    std::unordered_map<UnboxedHandle, BoxedHandle> mReverseMap GUARDED_BY(mMutex);

    struct DelayedRemove {
        BoxedHandle handle;
        std::function<void()> callback;
    };
    std::unordered_map<VkDevice, std::vector<DelayedRemove>> mDelayedRemoves GUARDED_BY(mMutex);

    // If true, `add()` will use and consume the handles in `mHandleReplayQueue`.
    // This is useful for snapshot loading when a explicit set of handles should
    // be used when replaying commands.
    bool mHandleReplay = false;
    std::deque<BoxedHandle> mHandleReplayQueue;
};

extern BoxedHandleManager sBoxedHandleManager;

#define DEFINE_BOXED_DISPATCHABLE_HANDLE_API_DECL(type)                                 \
    type new_boxed_##type(type underlying, VulkanDispatch* dispatch, bool ownDispatch); \
    void delete_##type(type boxed);                                                     \
    type unbox_##type(type boxed);                                                      \
    type try_unbox_##type(type boxed);                                                  \
    type unboxed_to_boxed_##type(type boxed);                                           \
    VulkanDispatch* dispatch_##type(type boxed);                                        \
    OrderMaintenanceInfo* ordmaint_##type(type boxed);                                  \
    VulkanMemReadingStream* readstream_##type(type boxed);

#define DEFINE_BOXED_NON_DISPATCHABLE_HANDLE_API_DECL(type)                                  \
    type new_boxed_non_dispatchable_##type(type underlying);                                 \
    void delete_##type(type boxed);                                                          \
    void delayed_delete_##type(type boxed, VkDevice device, std::function<void()> callback); \
    type unbox_##type(type boxed);                                                           \
    type try_unbox_##type(type boxed);                                                       \
    type unboxed_to_boxed_non_dispatchable_##type(type boxed);                               \
    void set_boxed_non_dispatchable_##type(type boxed, type underlying);

GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(DEFINE_BOXED_DISPATCHABLE_HANDLE_API_DECL)
GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(DEFINE_BOXED_NON_DISPATCHABLE_HANDLE_API_DECL)

}  // namespace vk
}  // namespace gfxstream
