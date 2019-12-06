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

class CbManagerGrallocImpl : public CbManager::CbManagerImpl {
public:
    CbManagerGrallocImpl(const hw_module_t* hwModule, alloc_device_t* allocDev)
      : mHwModule(hwModule), mAllocDev(allocDev) {}

    ~CbManagerGrallocImpl() {
        gralloc_close(mAllocDev);
    }

    const cb_handle_t* allocateBuffer(int width, int height, int format) {
        int ret;
        int stride;
        buffer_handle_t handle;

        ret = mAllocDev->alloc(mAllocDev, width, height, format,
                               GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_RENDER,
                               &handle, &stride);
        return ret ? nullptr : cb_handle_t::from(handle);
    }

    void freeBuffer(const cb_handle_t* h) {
        mAllocDev->free(mAllocDev, h);
    }

private:
    const hw_module_t* mHwModule;
    alloc_device_t* mAllocDev;
};

std::unique_ptr<CbManager::CbManagerImpl> buildGrallocImpl() {
    int ret;
    const hw_module_t* hwModule;

    ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &hwModule);
    if (ret) {
        return nullptr;
    }

    alloc_device_t* allocDev;
    ret = gralloc_open(hwModule, &allocDev);
    if (ret) {
        return nullptr;
    }

    return std::make_unique<CbManagerGrallocImpl>(hwModule, allocDev);
}
}  // namespace

CbManager::CbManager() : mImpl(buildGrallocImpl()) {}

}  // namespace android
