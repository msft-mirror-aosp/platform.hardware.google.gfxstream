/*
 * Copyright 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <log/log.h>

#include "RutabagaLayer.h"
#include "RutabagaVirtGpu.h"

namespace gfxstream {

RutabagaVirtGpuDevice::RutabagaVirtGpuDevice(std::shared_ptr<EmulatedVirtioGpu> emulation,
                                             uint32_t contextId, VirtGpuCapset capset)
    : VirtGpuDevice(capset), mEmulation(emulation), mContextId(contextId), mCapset(capset) {}

RutabagaVirtGpuDevice::~RutabagaVirtGpuDevice() { mEmulation->DestroyContext(mContextId); }

int64_t RutabagaVirtGpuDevice::getDeviceHandle() { return -1; }

VirtGpuCaps RutabagaVirtGpuDevice::getCaps() { return mEmulation->GetCaps(mCapset); }

VirtGpuBlobPtr RutabagaVirtGpuDevice::createBlob(const struct VirtGpuCreateBlob& blobCreate) {
    const auto resourceIdOpt = mEmulation->CreateBlob(mContextId, blobCreate);
    if (!resourceIdOpt) {
        return nullptr;
    }

    return VirtGpuBlobPtr(new RutabagaVirtGpuResource(
        mEmulation, *resourceIdOpt, RutabagaVirtGpuResource::ResourceType::kBlob, mContextId));
}

VirtGpuBlobPtr RutabagaVirtGpuDevice::createVirglBlob(uint32_t width, uint32_t height,
                                                      uint32_t virglFormat) {
    const auto resourceIdOpt = mEmulation->CreateVirglBlob(mContextId, width, height, virglFormat);
    if (!resourceIdOpt) {
        return nullptr;
    }

    return VirtGpuBlobPtr(new RutabagaVirtGpuResource(
        mEmulation, *resourceIdOpt, RutabagaVirtGpuResource::ResourceType::kPipe, mContextId));
}

int RutabagaVirtGpuDevice::execBuffer(struct VirtGpuExecBuffer& execbuffer,
                                      const VirtGpuBlob* blob) {
    std::optional<uint32_t> blobResourceId;
    if (blob) {
        blobResourceId = blob->getResourceHandle();
    }
    return mEmulation->ExecBuffer(mContextId, execbuffer, blobResourceId);
}

VirtGpuBlobPtr RutabagaVirtGpuDevice::importBlob(const struct VirtGpuExternalHandle&) {
    ALOGE("Unimplemented %s", __FUNCTION__);
    return nullptr;
}

}  // namespace gfxstream

VirtGpuDevice* createPlatformVirtGpuDevice(enum VirtGpuCapset capset, int) {
    std::shared_ptr<gfxstream::EmulatedVirtioGpu> emulation = gfxstream::EmulatedVirtioGpu::Get();
    if (!emulation) {
        ALOGE("Failed to create RutabagaVirtGpuDevice: failed to get emulation layer.");
        return nullptr;
    }

    const auto contextIdOp = emulation->CreateContext();
    if (!contextIdOp) {
        ALOGE("Failed to create RutabagaVirtGpuDevice: failed to create context.");
        return nullptr;
    }
    return new gfxstream::RutabagaVirtGpuDevice(emulation, *contextIdOp, capset);
}
