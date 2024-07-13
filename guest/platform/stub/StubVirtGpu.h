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

#pragma once

#include "VirtGpu.h"

class StubVirtGpuResource : public std::enable_shared_from_this<StubVirtGpuResource>,
                            public VirtGpuResource {
   public:
    StubVirtGpuResource(int64_t deviceHandle, uint32_t blobHandle, uint32_t resourceHandle,
                        uint64_t size);
    ~StubVirtGpuResource();

    uint32_t getResourceHandle() const override;
    uint32_t getBlobHandle() const override;
    int wait(void) override;

    VirtGpuResourceMappingPtr createMapping(void) override;
    int exportBlob(struct VirtGpuExternalHandle& handle) override;

    int transferFromHost(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;
    int transferToHost(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;

   private:
    // Not owned.  Really should use a ScopedFD for this, but doesn't matter since we have a
    // singleton deviceimplemenentation anyways.
    int64_t mDeviceHandle;

    uint32_t mBlobHandle;
    uint32_t mResourceHandle;
    uint64_t mSize;
};

class StubVirtGpuResourceMapping : public VirtGpuResourceMapping {
   public:
    StubVirtGpuResourceMapping(VirtGpuResourcePtr blob, uint8_t* ptr, uint64_t size);
    ~StubVirtGpuResourceMapping(void);

    uint8_t* asRawPtr(void) override;

  private:
   VirtGpuResourcePtr mBlob;
   uint8_t* mPtr;
   uint64_t mSize;
};

class StubVirtGpuDevice : public VirtGpuDevice {
  public:
    StubVirtGpuDevice(enum VirtGpuCapset capset);
    virtual ~StubVirtGpuDevice();

    int64_t getDeviceHandle(void) override;

    struct VirtGpuCaps getCaps(void) override;

    VirtGpuResourcePtr createBlob(const struct VirtGpuCreateBlob& blobCreate) override;
    VirtGpuResourcePtr createResource(uint32_t width, uint32_t height, uint32_t stride,
                                      uint32_t size, uint32_t virglFormat, uint32_t target,
                                      uint32_t bind);
    VirtGpuResourcePtr importBlob(const struct VirtGpuExternalHandle& handle) override;

    int execBuffer(struct VirtGpuExecBuffer& execbuffer, const VirtGpuResource* blob) override;

    virtual VirtGpuResourcePtr createColorBuffer(int width, int height, uint32_t glFormat);
    virtual VirtGpuResourcePtr createColorBuffer(int size);

   private:
    int64_t mDeviceHandle;

    struct VirtGpuCaps mCaps;
};
