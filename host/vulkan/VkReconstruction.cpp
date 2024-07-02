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
#include "render-utils/IOStream.h"
#include "VkDecoder.h"
#include "aemu/base/containers/EntityManager.h"

namespace gfxstream {
namespace vk {

#define DEBUG_RECONSTRUCTION 0

#if DEBUG_RECONSTRUCTION

#define DEBUG_RECON(fmt, ...) fprintf(stderr, "%s:%d " fmt "\n", __func__, __LINE__, ##__VA_ARGS__);

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

void VkReconstruction::save(android::base::Stream* stream) {
    DEBUG_RECON("start")

#if DEBUG_RECONSTRUCTION
    dump();
#endif

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
#if DEBUG_RECONSTRUCTION
                auto apiItem = mApiTrace.get(apiRef);
                DEBUG_RECON("adding handle 0x%lx API 0x%lx op code %d\n", handle.first, apiRef,
                            apiItem->opCode);
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
            auto item = mApiTrace.get(apiHandle);
            totalApiTraceSize += 4;                 // opcode
            totalApiTraceSize += 4;                 // buffer size of trace
            totalApiTraceSize += item->traceBytes;  // the actual trace
        }
    }

    DEBUG_RECON("total api trace size: %zu", totalApiTraceSize);

    std::vector<uint64_t> createdHandleBuffer;

    for (size_t i = 0; i < uniqApiRefsByTopoOrder.size(); ++i) {
        for (auto apiHandle : uniqApiRefsByTopoOrder[i]) {
            auto item = mApiTrace.get(apiHandle);
            for (auto createdHandle : item->createdHandles) {
                DEBUG_RECON("save handle: 0x%lx\n", createdHandle);
                createdHandleBuffer.push_back(createdHandle);
            }
        }
    }

    std::vector<uint8_t> apiTraceBuffer;
    apiTraceBuffer.resize(totalApiTraceSize);

    uint8_t* apiTracePtr = apiTraceBuffer.data();

    for (size_t i = 0; i < uniqApiRefsByTopoOrder.size(); ++i) {
        for (auto apiHandle : uniqApiRefsByTopoOrder[i]) {
            auto item = mApiTrace.get(apiHandle);
            // 4 bytes for opcode, and 4 bytes for saveBufferRaw's size field
            DEBUG_RECON("saving api handle 0x%lx op code %d\n", apiHandle, item->opCode);
            memcpy(apiTracePtr, &item->opCode, sizeof(uint32_t));
            apiTracePtr += 4;
            uint32_t traceBytesForSnapshot = item->traceBytes + 8;
            memcpy(apiTracePtr, &traceBytesForSnapshot,
                   sizeof(uint32_t));  // and 8 bytes for 'self' struct of { opcode, packetlen } as
                                       // that is what decoder expects
            apiTracePtr += 4;
            memcpy(apiTracePtr, item->trace.data(), item->traceBytes);
            apiTracePtr += item->traceBytes;
        }
    }

    DEBUG_RECON("created handle buffer size: %zu trace: %zu", createdHandleBuffer.size(),
                apiTraceBuffer.size());

    android::base::saveBufferRaw(stream, (char*)(createdHandleBuffer.data()),
                                 createdHandleBuffer.size() * sizeof(uint64_t));
    android::base::saveBufferRaw(stream, (char*)(apiTraceBuffer.data()), apiTraceBuffer.size());
}

class TrivialStream : public IOStream {
   public:
    TrivialStream() : IOStream(4) {}
    virtual ~TrivialStream() = default;

    void* allocBuffer(size_t minSize) {
        size_t allocSize = (m_bufsize < minSize ? minSize : m_bufsize);
        if (!m_buf) {
            m_buf = (unsigned char*)malloc(allocSize);
        } else if (m_bufsize < allocSize) {
            unsigned char* p = (unsigned char*)realloc(m_buf, allocSize);
            if (p != NULL) {
                m_buf = p;
                m_bufsize = allocSize;
            } else {
                ERR("realloc (%zu) failed\n", allocSize);
                free(m_buf);
                m_buf = NULL;
                m_bufsize = 0;
            }
        }

        return m_buf;
    }

    int commitBuffer(size_t size) {
        if (size == 0) return 0;
        return writeFully(m_buf, size);
    }

    int writeFully(const void* buf, size_t len) { return 0; }

    const unsigned char* readFully(void* buf, size_t len) { return NULL; }

    virtual void* getDmaForReading(uint64_t guest_paddr) { return nullptr; }
    virtual void unlockDma(uint64_t guest_paddr) {}

   protected:
    virtual const unsigned char* readRaw(void* buf, size_t* inout_len) { return nullptr; }
    virtual void onSave(android::base::Stream* stream) {}
    virtual unsigned char* onLoad(android::base::Stream* stream) { return nullptr; }
};

void VkReconstruction::load(android::base::Stream* stream, emugl::GfxApiLogger& gfxLogger,
                            emugl::HealthMonitor<>* healthMonitor) {
    DEBUG_RECON("start. assuming VkDecoderGlobalState has been cleared for loading already");
    mApiTrace.clear();
    mHandleReconstructions.clear();

    std::vector<uint8_t> createdHandleBuffer;
    std::vector<uint8_t> apiTraceBuffer;

    android::base::loadBuffer(stream, &createdHandleBuffer);
    android::base::loadBuffer(stream, &apiTraceBuffer);

    DEBUG_RECON("created handle buffer size: %zu trace: %zu", createdHandleBuffer.size(),
                apiTraceBuffer.size());

    uint32_t createdHandleBufferSize = createdHandleBuffer.size();

    mLoadedTrace.resize(4 + createdHandleBufferSize + apiTraceBuffer.size());

    unsigned char* finalTraceData = (unsigned char*)(mLoadedTrace.data());

    memcpy(finalTraceData, &createdHandleBufferSize, sizeof(uint32_t));
    memcpy(finalTraceData + 4, createdHandleBuffer.data(), createdHandleBufferSize);
    memcpy(finalTraceData + 4 + createdHandleBufferSize, apiTraceBuffer.data(),
           apiTraceBuffer.size());

    VkDecoder decoderForLoading;
    // A decoder that is set for snapshot load will load up the created handles first,
    // if any, allowing us to 'catch' the results as they are decoded.
    decoderForLoading.setForSnapshotLoad(true);
    TrivialStream trivialStream;

    DEBUG_RECON("start decoding trace");

    // TODO: This needs to be the puid seqno ptr
    auto resources = ProcessResources::create();
    VkDecoderContext context = {
        .processName = nullptr,
        .gfxApiLogger = &gfxLogger,
        .healthMonitor = healthMonitor,
    };
    decoderForLoading.decode(mLoadedTrace.data(), mLoadedTrace.size(), &trivialStream, resources.get(),
                             context);

    DEBUG_RECON("finished decoding trace");
}

VkReconstruction::ApiHandle VkReconstruction::createApiInfo() {
    auto handle = mApiTrace.add(ApiInfo(), 1);
    return handle;
}

void VkReconstruction::destroyApiInfo(VkReconstruction::ApiHandle h) {
    auto item = mApiTrace.get(h);

    if (!item) return;

    item->traceBytes = 0;
    item->createdHandles.clear();

    mApiTrace.remove(h);
}

VkReconstruction::ApiInfo* VkReconstruction::getApiInfo(VkReconstruction::ApiHandle h) {
    return mApiTrace.get(h);
}

void VkReconstruction::setApiTrace(VkReconstruction::ApiInfo* apiInfo, uint32_t opCode,
                                   const uint8_t* traceBegin, size_t traceBytes) {
    if (apiInfo->trace.size() < traceBytes) apiInfo->trace.resize(traceBytes);
    apiInfo->opCode = opCode;
    memcpy(apiInfo->trace.data(), traceBegin, traceBytes);
    apiInfo->traceBytes = traceBytes;
}

void VkReconstruction::dump() {
    fprintf(stderr, "%s: api trace dump\n", __func__);

    size_t traceBytesTotal = 0;

    mApiTrace.forEachLiveEntry_const(
        [&traceBytesTotal](bool live, uint64_t handle, const ApiInfo& info) {
            fprintf(stderr, "VkReconstruction::%s: api handle 0x%llx: %s\n", __func__,
                    (unsigned long long)handle, api_opcode_to_string(info.opCode));
            traceBytesTotal += info.traceBytes;
        });

    mHandleReconstructions.forEachLiveComponent_const(
        [this](bool live, uint64_t componentHandle, uint64_t entityHandle,
               const HandleWithStateReconstruction& reconstruction) {
            fprintf(stderr, "VkReconstruction::%s: %p handle 0x%llx api refs:\n", __func__, this,
                    (unsigned long long)entityHandle);
            for (const auto& state : reconstruction.states) {
                for (auto apiHandle : state.apiRefs) {
                    auto apiInfo = mApiTrace.get(apiHandle);
                    const char* apiName =
                        apiInfo ? api_opcode_to_string(apiInfo->opCode) : "unalloced";
                    fprintf(stderr, "VkReconstruction::%s:     0x%llx: %s\n", __func__,
                            (unsigned long long)apiHandle, apiName);
                    for (auto createdHandle : apiInfo->createdHandles) {
                        fprintf(stderr, "VkReconstruction::%s:         created 0x%llx\n", __func__,
                                (unsigned long long)createdHandle);
                    }
                }
            }
        });

    mHandleModifications.forEachLiveComponent_const([this](bool live, uint64_t componentHandle,
                                                           uint64_t entityHandle,
                                                           const HandleModification& modification) {
        fprintf(stderr, "VkReconstruction::%s: mod: %p handle 0x%llx api refs:\n", __func__, this,
                (unsigned long long)entityHandle);
        for (auto apiHandle : modification.apiRefs) {
            auto apiInfo = mApiTrace.get(apiHandle);
            const char* apiName = apiInfo ? api_opcode_to_string(apiInfo->opCode) : "unalloced";
            fprintf(stderr, "VkReconstruction::%s: mod:     0x%llx: %s\n", __func__,
                    (unsigned long long)apiHandle, apiName);
        }
    });

    fprintf(stderr, "%s: total trace bytes: %zu\n", __func__, traceBytesTotal);
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
        DEBUG_RECON("deleting api for 0x%lx\n", toProcess[i]);
        auto item = mHandleReconstructions.get(toProcess[i]);

        if (!item) continue;

        for (auto& state : item->states) {
            for (auto handle : state.apiRefs) {
                destroyApiInfo(handle);
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
        DEBUG_RECON("WARN: adding null parent item: 0x%lx\n", parentHandle);
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

    auto item = mApiTrace.get(apiHandle);

    if (!item) return;

    item->createdHandles.insert(item->createdHandles.end(), created, created + count);
    item->createdHandles.insert(item->createdHandles.end(), mExtraHandlesForNextApi.begin(),
                                mExtraHandlesForNextApi.end());
    mExtraHandlesForNextApi.clear();
}

void VkReconstruction::createExtraHandlesForNextApi(const uint64_t* created, uint32_t count) {
    mExtraHandlesForNextApi.assign(created, created + count);
}

void VkReconstruction::forEachHandleAddModifyApi(const uint64_t* toProcess, uint32_t count,
                                                 uint64_t apiHandle) {
    if (!toProcess) return;

    for (uint32_t i = 0; i < count; ++i) {
        mHandleModifications.add(toProcess[i], HandleModification());

        auto item = mHandleModifications.get(toProcess[i]);

        if (!item) continue;

        item->apiRefs.push_back(apiHandle);
    }
}

void VkReconstruction::forEachHandleClearModifyApi(const uint64_t* toProcess, uint32_t count) {
    if (!toProcess) return;

    for (uint32_t i = 0; i < count; ++i) {
        mHandleModifications.add(toProcess[i], HandleModification());

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
