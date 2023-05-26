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

#include "RenderThreadInfoMagma.h"

#include "host-common/GfxstreamFatalError.h"

static thread_local RenderThreadInfoMagma* tlThreadInfo = nullptr;

RenderThreadInfoMagma::RenderThreadInfoMagma(uint32_t context_id) {
    if (tlThreadInfo != nullptr) {
      GFXSTREAM_ABORT(emugl::FatalError(emugl::ABORT_REASON_OTHER))
        << "Attempted to set thread local Magma render thread info twice.";
    }
    tlThreadInfo = this;
    mMagmaDec = gfxstream::magma::Decoder::create(context_id);
}

RenderThreadInfoMagma::~RenderThreadInfoMagma() {
    tlThreadInfo = nullptr;
}

RenderThreadInfoMagma* RenderThreadInfoMagma::get() {
    return tlThreadInfo;
}
