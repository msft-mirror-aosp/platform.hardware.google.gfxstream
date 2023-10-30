// Copyright 2023 The Android Open Source Project
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

#pragma once

#include "Gralloc.h"

namespace gfxstream {

class MinigbmGralloc : public Gralloc {
   public:
    uint32_t createColorBuffer(void* rcEnc, int width, int height,
                               uint32_t glformat) override;

    int allocate(uint32_t width,
                 uint32_t height,
                 uint32_t format,
                 uint64_t usage,
                 AHardwareBuffer** outputAhb) override;

    void acquire(AHardwareBuffer* ahb) override;
    void release(AHardwareBuffer* ahb) override;

    uint32_t getHostHandle(native_handle_t const* handle) override;
    uint32_t getHostHandle(const AHardwareBuffer* handle) override;

    int getFormat(const native_handle_t* handle) override;
    int getFormat(const AHardwareBuffer* handle) override;

    uint32_t getFormatDrmFourcc(const native_handle_t* handle) override;
    uint32_t getFormatDrmFourcc(const AHardwareBuffer* handle) override;

    size_t getAllocatedSize(const native_handle_t* handle) override;
    size_t getAllocatedSize(const AHardwareBuffer* handle) override;

    void setFd(int fd) { m_fd = fd; }

   private:
    int m_fd;
};

}  // namespace gfxstream
