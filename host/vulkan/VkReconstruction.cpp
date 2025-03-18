// Copyright (C) 2019 The Android Open Source Project
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
#include "VkReconstruction.h"

#include <string.h>

#include <unordered_map>

#include "FrameBuffer.h"
#include "VkDecoder.h"
#include "aemu/base/containers/EntityManager.h"

namespace gfxstream {
namespace vk {
namespace {

uint32_t GetOpcode(const VkSnapshotApiCallInfo& info) {
    if (info.packet.size() <= 4) return -1;

    return *(reinterpret_cast<const uint32_t*>(info.packet.data()));
}

}  // namespace

#define DEBUG_RECONSTRUCTION 0

#if DEBUG_RECONSTRUCTION

#define DEBUG_RECON(fmt, ...) INFO(fmt, ##__VA_ARGS__);

#else

#define DEBUG_RECON(fmt, ...)

#endif

VkReconstruction::VkReconstruction() = default;

std::vector<VkReconstruction::HandleWithState> typeTagSortedHandles(
    const std::vector<VkReconstruction::HandleWithState>& handles) {
    using EntityManagerTypeForHandles = android::base::EntityManager<32, 16, 16, int>;

    std::vector<VkReconstruction::HandleWithState> res = handles;

    std::sort(res.begin(), res.end(),
              [](const VkReconstruction::HandleWithState& lhs,
                 const VkReconstruction::HandleWithState& rhs) {
                  if (lhs.second != rhs.second) {
                      return lhs.second < rhs.second;
                  }
                  return EntityManagerTypeForHandles::getHandleType(lhs.first) <
                         EntityManagerTypeForHandles::getHandleType(rhs.first);
              });

    return res;
}

void VkReconstruction::clear() {
    mApiCallManager.clear();
    mHandleReconstructions.clear();
}

void VkReconstruction::saveReplayBuffers(android::base::Stream* stream) {
    DEBUG_RECON("start")

#if DEBUG_RECONSTRUCTION
    dump();
#endif

    std::unordered_set<uint64_t> savedApis;

    std::unordered_map<HandleWithState, int, HandleWithStateHash> totalParents;
    std::vector<HandleWithState> next;

    mHandleReconstructions.forEachLiveComponent_const(
        [&totalParents, &next](bool live, uint64_t componentHandle, uint64_t entityHandle,
                               const HandleWithStateReconstruction& item) {
            for (int state = BEGIN; state < HANDLE_STATE_COUNT; state++) {
                const auto& parents = item.states[state].parentHandles;
                HandleWithState handleWithState = {entityHandle, static_cast<HandleState>(state)};
                totalParents[handleWithState] = parents.size();
                if (parents.empty()) {
                    next.push_back(handleWithState);
                }
            }
        });

    std::vector<std::vector<HandleWithState>> handlesByTopoOrder;

    while (!next.empty()) {
        next = typeTagSortedHandles(next);
        handlesByTopoOrder.push_back(std::move(next));
        const std::vector<HandleWithState>& current = handlesByTopoOrder.back();
        for (const auto& handle : current) {
            const auto& item = mHandleReconstructions.get(handle.first)->states[handle.second];
            for (const auto& childHandle : item.childHandles) {
                if (--totalParents[childHandle] == 0) {
                    next.push_back(childHandle);
                }
            }
        }
    }

    std::vector<std::vector<uint64_t>> uniqApiRefsByTopoOrder;
    uniqApiRefsByTopoOrder.reserve(handlesByTopoOrder.size() + 1);
    for (const auto& handles : handlesByTopoOrder) {
        std::vector<uint64_t> nextApis;
        for (const auto& handle : handles) {
            auto item = mHandleReconstructions.get(handle.first)->states[handle.second];
            for (uint64_t apiRef : item.apiRefs) {
                auto apiItem = mApiCallManager.get(apiRef);
                if (!apiItem) continue;
                if (savedApis.find(apiRef) != savedApis.end()) continue;
                savedApis.insert(apiRef);
#if DEBUG_RECONSTRUCTION
                DEBUG_RECON("adding handle 0x%lx API 0x%lx op code %d", handle.first, apiRef,
                            GetOpcode(*apiItem));
#endif
                nextApis.push_back(apiRef);
            }
        }
        uniqApiRefsByTopoOrder.push_back(std::move(nextApis));
    }

    uniqApiRefsByTopoOrder.push_back(getOrderedUniqueModifyApis());

    size_t totalApiTraceSize = 0;  // 4 bytes to store size of created handles

    for (size_t i = 0; i < uniqApiRefsByTopoOrder.size(); ++i) {
        for (auto apiHandle : uniqApiRefsByTopoOrder[i]) {
            const VkSnapshotApiCallInfo* info = mApiCallManager.get(apiHandle);
            totalApiTraceSize += info->packet.size();
        }
    }

    DEBUG_RECON("total api trace size: %zu", totalApiTraceSize);

    std::vector<uint64_t> createdHandleBuffer;

    for (size_t i = 0; i < uniqApiRefsByTopoOrder.size(); ++i) {
        for (auto apiHandle : uniqApiRefsByTopoOrder[i]) {
            auto item = mApiCallManager.get(apiHandle);
            for (auto createdHandle : item->createdHandles) {
                DEBUG_RECON("save handle: 0x%lx", createdHandle);
                createdHandleBuffer.push_back(createdHandle);
            }
        }
    }

    std::vector<uint8_t> apiTraceBuffer;
    apiTraceBuffer.resize(totalApiTraceSize);

    uint8_t* apiTracePtr = apiTraceBuffer.data();

    for (size_t i = 0; i < uniqApiRefsByTopoOrder.size(); ++i) {
        for (auto apiHandle : uniqApiRefsByTopoOrder[i]) {
            auto item = mApiCallManager.get(apiHandle);
            // 4 bytes for opcode, and 4 bytes for saveBufferRaw's size field
            DEBUG_RECON("saving api handle 0x%lx op code %d", apiHandle, GetOpcode(*item));
            memcpy(apiTracePtr, item->packet.data(), item->packet.size());
            apiTracePtr += item->packet.size();
        }
    }

    DEBUG_RECON("created handle buffer size: %zu trace: %zu", createdHandleBuffer.size(),
                apiTraceBuffer.size());

    android::base::saveBuffer(stream, createdHandleBuffer);
    android::base::saveBuffer(stream, apiTraceBuffer);
}

/*static*/
void VkReconstruction::loadReplayBuffers(android::base::Stream* stream,
                                         std::vector<uint64_t>* outHandleBuffer,
                                         std::vector<uint8_t>* outDecoderBuffer) {
    DEBUG_RECON("starting to unpack decoder replay buffer");

    android::base::loadBuffer(stream, outHandleBuffer);
    android::base::loadBuffer(stream, outDecoderBuffer);

    DEBUG_RECON("finished unpacking decoder replay buffer");
}

VkSnapshotApiCallInfo* VkReconstruction::createApiCallInfo() {
    VkSnapshotApiCallHandle handle = mApiCallManager.add(VkSnapshotApiCallInfo(), 1);

    auto* info = mApiCallManager.get(handle);
    info->handle = handle;
    return info;
}

void VkReconstruction::removeHandleFromApiInfo(VkSnapshotApiCallHandle h, uint64_t toRemove) {
    auto vk_item = mHandleReconstructions.get(toRemove);
    if (!vk_item) return;
    auto apiInfo = mApiCallManager.get(h);
    if (!apiInfo) return;

    auto& handles = apiInfo->createdHandles;
    auto it = std::find(handles.begin(), handles.end(), toRemove);

    if (it != handles.end()) {
        handles.erase(it);
    }
    DEBUG_RECON("removed 1 vk handle  0x%llx from apiInfo  0x%llx, now it has %d left",
                (unsigned long long)toRemove, (unsigned long long)h, (int)handles.size());
}

void VkReconstruction::destroyApiCallInfo(VkSnapshotApiCallHandle h) {
    auto item = mApiCallManager.get(h);

    if (!item) return;

    if (!item->createdHandles.empty()) return;

    item->createdHandles.clear();

    mApiCallManager.remove(h);
}

void VkReconstruction::destroyApiCallInfoIfUnused(VkSnapshotApiCallInfo* info) {
    if (!info) return;
    auto handle = info->handle;
    auto currentInfo = mApiCallManager.get(handle);
    if (!currentInfo) return;

    if (currentInfo->packet.empty()) {
        mApiCallManager.remove(handle);
        return;
    }

    if (!info->extraCreatedHandles.empty()) {
        currentInfo->createdHandles.insert(currentInfo->createdHandles.end(), info->extraCreatedHandles.begin(),
                                    info->extraCreatedHandles.end());
        info->extraCreatedHandles.clear();
    }
}

VkSnapshotApiCallInfo* VkReconstruction::getApiInfo(VkSnapshotApiCallHandle h) {
    return mApiCallManager.get(h);
}

void VkReconstruction::setApiTrace(VkSnapshotApiCallInfo* apiInfo, const uint8_t* packet,
                                   size_t packetLenBytes) {
    auto* info = mApiCallManager.get(apiInfo->handle);
    if(info) {
        info->packet.assign(packet, packet + packetLenBytes);
    }
}

void VkReconstruction::dump() {
    INFO("%s: api trace dump", __func__);

    size_t traceBytesTotal = 0;

    mApiCallManager.forEachLiveEntry_const(
        [&traceBytesTotal](bool live, uint64_t handle, const VkSnapshotApiCallInfo& info) {
            const uint32_t opcode = GetOpcode(info);
            INFO("VkReconstruction::%s: api handle 0x%llx: %s", __func__,
                 (unsigned long long)handle, api_opcode_to_string(opcode));
            traceBytesTotal += info.packet.size();
        });

    mHandleReconstructions.forEachLiveComponent_const(
        [this](bool live, uint64_t componentHandle, uint64_t entityHandle,
               const HandleWithStateReconstruction& reconstruction) {
            INFO("VkReconstruction::%s: %p handle 0x%llx api refs:", __func__, this,
                    (unsigned long long)entityHandle);
            for (const auto& state : reconstruction.states) {
                for (auto apiHandle : state.apiRefs) {
                    auto apiInfo = mApiCallManager.get(apiHandle);
                    const char* apiName =
                        apiInfo ? api_opcode_to_string(GetOpcode(*apiInfo)) : "unalloced";
                    INFO("VkReconstruction::%s:     0x%llx: %s", __func__,
                            (unsigned long long)apiHandle, apiName);
                    for (auto createdHandle : apiInfo->createdHandles) {
                        INFO("VkReconstruction::%s:         created 0x%llx", __func__,
                                (unsigned long long)createdHandle);
                    }
                }
            }
        });

    mHandleModifications.forEachLiveComponent_const([this](bool live, uint64_t componentHandle,
                                                           uint64_t entityHandle,
                                                           const HandleModification& modification) {
        INFO("VkReconstruction::%s: mod: %p handle 0x%llx api refs:", __func__, this,
                (unsigned long long)entityHandle);
        for (auto apiHandle : modification.apiRefs) {
            auto apiInfo = mApiCallManager.get(apiHandle);
            const char* apiName = apiInfo ? api_opcode_to_string(GetOpcode(*apiInfo)) : "unalloced";
            INFO("VkReconstruction::%s: mod:     0x%llx: %s", __func__,
                    (unsigned long long)apiHandle, apiName);
        }
    });

    INFO("%s: total trace bytes: %zu", __func__, traceBytesTotal);
}

void VkReconstruction::addHandles(const uint64_t* toAdd, uint32_t count) {
    if (!toAdd) return;

    for (uint32_t i = 0; i < count; ++i) {
        DEBUG_RECON("add 0x%llx", (unsigned long long)toAdd[i]);
        mHandleReconstructions.add(toAdd[i], HandleWithStateReconstruction());
    }
}

void VkReconstruction::removeHandles(const uint64_t* toRemove, uint32_t count, bool recursive) {
    if (!toRemove) return;

    for (uint32_t i = 0; i < count; ++i) {
        DEBUG_RECON("remove 0x%llx", (unsigned long long)toRemove[i]);
        auto item = mHandleReconstructions.get(toRemove[i]);
        // Delete can happen in arbitrary order.
        // It might delete the parents before children, which will automatically remove
        // the name.
        if (!item) continue;
        // Break circuler references
        if (item->destroying) continue;
        item->destroying = true;
        if (!recursive) {
            bool couldDestroy = true;
            for (const auto& state : item->states) {
                if (!state.childHandles.size()) {
                    continue;
                }
                couldDestroy = false;
                break;
            }
            // TODO(b/330769702): perform delayed destroy when all children are destroyed.
            if (couldDestroy) {
                forEachHandleDeleteApi(toRemove + i, 1);
                mHandleReconstructions.remove(toRemove[i]);
            } else {
                DEBUG_RECON("delay destroy of 0x%lx, TODO: actually destroy it", toRemove[i]);
                item->delayed_destroy = true;
                item->destroying = false;
            }
            continue;
        }
        for (size_t j = 0; j < item->states.size(); j++) {
            for (const auto& parentHandle : item->states[j].parentHandles) {
                auto parentItem = mHandleReconstructions.get(parentHandle.first);
                if (!parentItem) {
                    continue;
                }
                parentItem->states[parentHandle.second].childHandles.erase(
                    {toRemove[i], static_cast<HandleState>(j)});
            }
            item->states[j].parentHandles.clear();
            std::vector<uint64_t> childHandles;
            for (const auto& childHandle : item->states[j].childHandles) {
                if (childHandle.second == CREATED) {
                    childHandles.push_back(childHandle.first);
                }
            }
            item->states[j].childHandles.clear();
            removeHandles(childHandles.data(), childHandles.size());
        }
        forEachHandleDeleteApi(toRemove + i, 1);
        mHandleReconstructions.remove(toRemove[i]);
    }
}

void VkReconstruction::forEachHandleAddApi(const uint64_t* toProcess, uint32_t count,
                                           uint64_t apiHandle, HandleState state) {
    if (!toProcess) return;

    for (uint32_t i = 0; i < count; ++i) {
        auto item = mHandleReconstructions.get(toProcess[i]);
        if (!item) continue;

        item->states[state].apiRefs.push_back(apiHandle);
        DEBUG_RECON("handle 0x%lx state %d added api 0x%lx", toProcess[i], state, apiHandle);
    }
}

void VkReconstruction::forEachHandleDeleteApi(const uint64_t* toProcess, uint32_t count) {
    if (!toProcess) return;

    for (uint32_t i = 0; i < count; ++i) {
        DEBUG_RECON("deleting api for 0x%lx", toProcess[i]);
        auto item = mHandleReconstructions.get(toProcess[i]);

        if (!item) continue;

        for (auto& state : item->states) {
            for (auto handle : state.apiRefs) {
                removeHandleFromApiInfo(handle, toProcess[i]);
                destroyApiCallInfo(handle);
            }
            state.apiRefs.clear();
        }

        auto modifyItem = mHandleModifications.get(toProcess[i]);

        if (!modifyItem) continue;

        modifyItem->apiRefs.clear();
    }
}

void VkReconstruction::addHandleDependency(const uint64_t* handles, uint32_t count,
                                           uint64_t parentHandle, HandleState childState,
                                           HandleState parentState) {
    if (!handles) return;

    if (!parentHandle) return;

    auto parentItem = mHandleReconstructions.get(parentHandle);

    if (!parentItem) {
        DEBUG_RECON("WARN: adding null parent item: 0x%lx", parentHandle);
        return;
    }
    auto& parentItemState = parentItem->states[parentState];

    for (uint32_t i = 0; i < count; ++i) {
        auto childItem = mHandleReconstructions.get(handles[i]);
        if (!childItem) {
            continue;
        }
        parentItemState.childHandles.insert({handles[i], static_cast<HandleState>(childState)});
        childItem->states[childState].parentHandles.push_back(
            {parentHandle, static_cast<HandleState>(parentState)});
        DEBUG_RECON("Child handle 0x%lx state %d depends on parent handle 0x%lx state %d",
                    handles[i], childState, parentHandle, parentState);
    }
}

void VkReconstruction::setCreatedHandlesForApi(uint64_t apiHandle, const uint64_t* created,
                                               uint32_t count) {
    if (!created) return;

    auto item = mApiCallManager.get(apiHandle);

    if (!item) return;

    item->createdHandles.insert(item->createdHandles.end(), created, created + count);
}

void VkReconstruction::forEachHandleAddModifyApi(const uint64_t* toProcess, uint32_t count,
                                                 uint64_t apiHandle) {
    if (!toProcess) return;

    for (uint32_t i = 0; i < count; ++i) {
        auto item = mHandleModifications.get(toProcess[i]);
        if (!item) {
            mHandleModifications.add(toProcess[i], HandleModification());
            item = mHandleModifications.get(toProcess[i]);
        }

        if (!item) continue;

        item->apiRefs.push_back(apiHandle);
    }
}

void VkReconstruction::forEachHandleClearModifyApi(const uint64_t* toProcess, uint32_t count) {
    if (!toProcess) return;

    for (uint32_t i = 0; i < count; ++i) {
        auto item = mHandleModifications.get(toProcess[i]);

        if (!item) continue;

        item->apiRefs.clear();
    }
}

std::vector<uint64_t> VkReconstruction::getOrderedUniqueModifyApis() const {
    std::vector<HandleModification> orderedModifies;

    // Now add all handle modifications to the trace, ordered by the .order field.
    mHandleModifications.forEachLiveComponent_const(
        [&orderedModifies](bool live, uint64_t componentHandle, uint64_t entityHandle,
                           const HandleModification& mod) { orderedModifies.push_back(mod); });

    // Sort by the |order| field for each modify API
    // since it may be important to apply modifies in a particular
    // order (e.g., when dealing with descriptor set updates
    // or commands in a command buffer).
    std::sort(orderedModifies.begin(), orderedModifies.end(),
              [](const HandleModification& lhs, const HandleModification& rhs) {
                  return lhs.order < rhs.order;
              });

    std::unordered_set<uint64_t> usedModifyApis;
    std::vector<uint64_t> orderedUniqueModifyApis;

    for (const auto& mod : orderedModifies) {
        for (auto apiRef : mod.apiRefs) {
            if (usedModifyApis.find(apiRef) == usedModifyApis.end()) {
                orderedUniqueModifyApis.push_back(apiRef);
                usedModifyApis.insert(apiRef);
            }
        }
    }

    return orderedUniqueModifyApis;
}

}  // namespace vk
}  // namespace gfxstream
