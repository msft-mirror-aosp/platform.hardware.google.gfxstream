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

#ifndef ANDROID_GOLDFISH_OPENGL_SYSTEM_CBMANAGER_CBMANAGER_H
#define ANDROID_GOLDFISH_OPENGL_SYSTEM_CBMANAGER_CBMANAGER_H

#include <memory>
#include "gralloc_cb.h"

namespace android {

class CbManager {
public:
    CbManager();

    class CbManagerImpl {
    public:
        virtual ~CbManagerImpl() {}
        virtual const cb_handle_t* allocateBuffer(int width,
                                                  int height,
                                                  int format) = 0;
        virtual void freeBuffer(const cb_handle_t* h) = 0;
    };

    const cb_handle_t* allocateBuffer(int width, int height, int format) {
        return mImpl->allocateBuffer(width, height, format);
    }

    void freeBuffer(const cb_handle_t* h) {
        mImpl->freeBuffer(h);
    }

private:
    std::unique_ptr<CbManagerImpl> mImpl;
};

}  // namespace android

#endif  // ANDROID_GOLDFISH_OPENGL_SYSTEM_CBMANAGER_CBMANAGER_H
