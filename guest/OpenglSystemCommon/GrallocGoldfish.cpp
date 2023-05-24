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

#include "GrallocGoldfish.h"

#include <gralloc_cb_bp.h>

namespace gfxstream {

uint32_t GoldfishGralloc::createColorBuffer(renderControl_client_context_t* rcEnc, int width,
                                            int height, uint32_t glformat) {
    return rcEnc->rcCreateColorBuffer(rcEnc, width, height, glformat);
}

uint32_t GoldfishGralloc::getHostHandle(native_handle_t const* handle) {
    return cb_handle_t::from(handle)->hostHandle;
}

int GoldfishGralloc::getFormat(native_handle_t const* handle) {
    return cb_handle_t::from(handle)->format;
}

size_t GoldfishGralloc::getAllocatedSize(native_handle_t const* handle) {
    return static_cast<size_t>(cb_handle_t::from(handle)->allocatedSize());
}

}  // namespace gfxstream
