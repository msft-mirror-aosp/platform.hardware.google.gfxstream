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

#include <memory>
#include <optional>

#include "VirtGpu.h"

namespace gfxstream {

// Emulates parts of the Linux Virtio GPU kernel module and parts of
// a virtual machine manager to allow speaking directly to the Gfxstream
// host server via rutabaga.
class EmulatedVirtioGpu {
  public:
    static EmulatedVirtioGpu& Get();
    static void Reset();

    bool Init(bool withGl, bool withVk, bool withVkSnapshots);

    VirtGpuCaps GetCaps(VirtGpuCapset capset);

    std::optional<uint32_t> CreateContext();
    void DestroyContext(uint32_t contextId);

    std::optional<uint32_t> CreateBlob(uint32_t contextId,
                                       const struct VirtGpuCreateBlob& params);
    std::optional<uint32_t> CreateVirglBlob(uint32_t contextId,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t virglFormat);

    void DestroyResource(uint32_t contextId,
                         uint32_t resourceId);

    uint8_t* Map(uint32_t resourceId);
    void Unmap(uint32_t resourceId);

    int ExecBuffer(uint32_t contextId,
                   struct VirtGpuExecBuffer& execbuffer,
                   std::optional<uint32_t> blobResourceId);

    int Wait(uint32_t resourceId);

    int TransferFromHost(uint32_t contextId,
                         uint32_t resourceId,
                         uint32_t transferOffset,
                         uint32_t transferSize);
    int TransferToHost(uint32_t contextId,
                       uint32_t resourceId,
                       uint32_t transferOffset,
                       uint32_t transferSize);

    void SignalEmulatedFence(int fenceId);

    int WaitOnEmulatedFence(int fenceAsFileDescriptor, int timeoutMilliseconds);

  private:
    EmulatedVirtioGpu();

    class EmulatedVirtioGpuImpl;
    std::unique_ptr<EmulatedVirtioGpuImpl> mImpl;
};

}  // namespace gfxstream
