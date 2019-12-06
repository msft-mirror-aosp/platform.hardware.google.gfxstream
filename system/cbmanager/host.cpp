/*
 * Copyright 2019 The Android Open Source Project
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

#include <hardware/gralloc.h>
#include "cbmanager.h"

namespace android {
namespace {

class CbManagerHostImpl : public CbManager::CbManagerImpl {
public:
    CbManagerHostImpl() {}

    ~CbManagerHostImpl() {}

    const cb_handle_t* allocateBuffer(int width, int height, int format) {
        return nullptr;
    }

    void freeBuffer(const cb_handle_t* h) {
    }

private:
};

std::unique_ptr<CbManager::CbManagerImpl> buildHostImpl() {
    return std::make_unique<CbManagerHostImpl>();
}
}  // namespace

CbManager::CbManager() : mImpl(buildHostImpl()) {}

}  // namespace android
