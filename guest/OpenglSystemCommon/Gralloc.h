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

#include <cutils/native_handle.h>

#include "renderControl_enc.h"

namespace gfxstream {

// Abstraction for gralloc handle conversion
class Gralloc {
   public:
    virtual ~Gralloc() {}

    virtual uint32_t createColorBuffer(renderControl_client_context_t* rcEnc, int width, int height,
                                       uint32_t glformat) = 0;
    virtual uint32_t getHostHandle(native_handle_t const* handle) = 0;
    virtual int getFormat(native_handle_t const* handle) = 0;
    virtual uint32_t getFormatDrmFourcc(native_handle_t const* /*handle*/) {
        // Equal to DRM_FORMAT_INVALID -- see <drm_fourcc.h>
        return 0;
    }
    virtual size_t getAllocatedSize(native_handle_t const* handle) = 0;
};

}  // namespace gfxstream
