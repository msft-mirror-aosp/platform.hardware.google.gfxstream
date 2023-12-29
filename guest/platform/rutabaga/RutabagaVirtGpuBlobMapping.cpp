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

#include "RutabagaLayer.h"
#include "RutabagaVirtGpu.h"

namespace gfxstream {

RutabagaVirtGpuBlobMapping::RutabagaVirtGpuBlobMapping(VirtGpuBlobPtr blob, uint8_t* mapped)
    : mBlob(blob), mMapped(mapped) {}

RutabagaVirtGpuBlobMapping::~RutabagaVirtGpuBlobMapping(void) {
    EmulatedVirtioGpu::Get().Unmap(mBlob->getResourceHandle());
}

uint8_t* RutabagaVirtGpuBlobMapping::asRawPtr(void) { return mMapped; }

}  // namespace gfxstream
