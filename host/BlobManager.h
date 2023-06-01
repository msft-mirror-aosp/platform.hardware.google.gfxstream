// Copyright 2019 The Android Open Source Project
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

#include <inttypes.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "aemu/base/Compiler.h"
#include "aemu/base/ManagedDescriptor.hpp"

// A global mapping from opaque host memory IDs to host virtual
// addresses/sizes.  This is so that the guest doesn't have to know the host
// virtual address to be able to map them. However, we do also provide a
// mechanism for obtaining the offsets into page for such buffers (as the guest
// does need to know those).
//
// This is currently used only in conjunction with virtio-gpu-next and Vulkan /
// address space device, though there are possible other consumers of this, so
// it becomes a global object. It exports methods into VmOperations.

using android::base::ManagedDescriptor;

namespace gfxstream {

// Caching types
#define MAP_CACHE_MASK 0x0f
#define MAP_CACHE_NONE 0x00
#define MAP_CACHE_CACHED 0x01
#define MAP_CACHE_UNCACHED 0x02
#define MAP_CACHE_WC 0x03

#define STREAM_MEM_HANDLE_TYPE_OPAQUE_FD 0x1
#define STREAM_MEM_HANDLE_TYPE_DMABUF 0x2
#define STREAM_MEM_HANDLE_TYPE_OPAQUE_WIN32 0x3
#define STREAM_MEM_HANDLE_TYPE_SHM 0x4
#define STREAM_MEM_HANDLE_TYPE_ZIRCON 0x5
#define STREAM_FENCE_HANDLE_TYPE_OPAQUE_FD 0x6
#define STREAM_FENCE_HANDLE_TYPE_SYNC_FD 0x7
#define STREAM_FENCE_HANDLE_TYPE_OPAQUE_WIN32 0x8
#define STREAM_FENCE_HANDLE_TYPE_ZIRCON 0x9

// A struct describing the information about host memory associated
// with a host memory id. Used with virtio-gpu-next.
struct HostMemInfo {
    void* addr;
    uint32_t caching;
};

struct VulkanInfo {
    uint32_t memoryIndex;
    uint8_t deviceUUID[16];
    uint8_t driverUUID[16];
};

struct ManagedDescriptorInfo {
    ManagedDescriptor descriptor;
    uint32_t handleType;
    uint32_t caching;
    std::optional<VulkanInfo> vulkanInfoOpt;
};

class BlobManager {
   public:
    BlobManager() = default;

    static BlobManager* get();

    void addMapping(uint32_t ctx_id, uint64_t blobId, void* addr, uint32_t caching);
    std::optional<HostMemInfo> removeMapping(uint32_t ctx_id, uint64_t blobId);

    void addDescriptorInfo(uint32_t ctx_id, uint64_t blobId, ManagedDescriptor descriptor,
                           uint32_t handleType, uint32_t caching,
                           std::optional<VulkanInfo> vulkanInfoOpt);
    std::optional<ManagedDescriptorInfo> removeDescriptorInfo(uint32_t ctx_id, uint64_t blobId);

   private:
    // Only for pairs of std::hash-able types for simplicity.
    // You can of course template this struct to allow other hash functions
    struct pair_hash {
        template <class T1, class T2>
        std::size_t operator()(const std::pair<T1, T2>& p) const {
            auto h1 = std::hash<T1>{}(p.first);
            auto h2 = std::hash<T2>{}(p.second);

            // Mainly for demonstration purposes, i.e. works but is overly simple
            // In the real world, use sth. like boost.hash_combine
            return h1 ^ h2;
        }
    };

    std::mutex mLock;
    std::unordered_map<std::pair<uint32_t, uint64_t>, HostMemInfo, pair_hash> mHostMemInfos;
    std::unordered_map<std::pair<uint32_t, uint64_t>, ManagedDescriptorInfo, pair_hash>
        mDescriptorInfos;
    DISALLOW_COPY_ASSIGN_AND_MOVE(BlobManager);
};

}  // namespace gfxstream
