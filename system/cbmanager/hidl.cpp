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

#include <cutils/native_handle.h>
#include <android/hardware/graphics/allocator/2.0/IAllocator.h>
#include <android/hardware/graphics/mapper/2.0/IMapper.h>
#include <log/log.h>

#include "cbmanager.h"
#include "debug.h"

namespace android {
namespace {
using hardware::hidl_handle;
using hardware::hidl_vec;

namespace IMapper2ns = hardware::graphics::mapper::V2_0;
namespace IAllocator2ns = hardware::graphics::allocator::V2_0;

class CbManagerHidlV2Impl : public CbManager::CbManagerImpl {
public:
    typedef CbManager::BufferUsage BufferUsage;
    typedef CbManager::PixelFormat PixelFormat;
    typedef hardware::hidl_bitfield<BufferUsage> BufferUsageBits;

    CbManagerHidlV2Impl(sp<IMapper2ns::IMapper> mapper,
                        sp<IAllocator2ns::IAllocator> allocator)
      : mMapper(mapper), mAllocator(allocator) {}

    const cb_handle_t* allocateBuffer(int width, int height,
                                      PixelFormat format, BufferUsageBits usage) {
        using IMapper2ns::Error;
        using IMapper2ns::BufferDescriptor;

        IMapper2ns::IMapper::BufferDescriptorInfo descriptor_info;
        descriptor_info.width = width;
        descriptor_info.height = height;
        descriptor_info.layerCount = 1;
        descriptor_info.format = format;
        descriptor_info.usage = usage;
        Error hidl_err = Error::NONE;

        BufferDescriptor descriptor;
        mMapper->createDescriptor(descriptor_info,
                                  [&](const Error &_error,
                                      const BufferDescriptor &_descriptor) {
            hidl_err = _error;
            descriptor = _descriptor;
        });
        if (hidl_err != Error::NONE) {
            RETURN_ERROR(nullptr);
        }

        hidl_handle raw_handle = nullptr;
        mAllocator->allocate(descriptor, 1,
                             [&](const Error &_error,
                                 uint32_t _stride,
                                 const hidl_vec<hidl_handle> &_buffers) {
            hidl_err = _error;
            (void)_stride;
            raw_handle = _buffers[0];
        });
        if (hidl_err != Error::NONE) {
            RETURN_ERROR(nullptr);
        }

        const cb_handle_t *buf = nullptr;
        mMapper->importBuffer(raw_handle, [&](const Error &_error,
                                              void *_buf) {
            hidl_err = _error;
            buf = cb_handle_t::from(_buf);
        });
        if (hidl_err != Error::NONE) {
            RETURN_ERROR(nullptr);
        }

        RETURN(buf);
    }

    void freeBuffer(const cb_handle_t* _h) {
        using IMapper2ns::Error;

        cb_handle_t* h = const_cast<cb_handle_t*>(_h);

        mMapper->freeBuffer(h);
        native_handle_close(h);
        native_handle_delete(h);
    }

private:
    const sp<IMapper2ns::IMapper> mMapper;
    const sp<IAllocator2ns::IAllocator> mAllocator;
};

std::unique_ptr<CbManager::CbManagerImpl> buildHidlImpl() {
    sp<IMapper2ns::IMapper> mapper =
        IMapper2ns::IMapper::getService();
    if (!mapper) {
        ALOGE("%s:%d: no IMapper implementation found", __func__, __LINE__);
        RETURN_ERROR(nullptr);
    }

    sp<IAllocator2ns::IAllocator> allocator =
        IAllocator2ns::IAllocator::getService();
    if (!allocator) {
        ALOGE("%s:%d: no IAllocator implementation found", __func__, __LINE__);
        RETURN_ERROR(nullptr);
    }

    return std::make_unique<CbManagerHidlV2Impl>(mapper, allocator);
}

}  // namespace

CbManager::CbManager() : mImpl(buildHidlImpl()) {}

}  // namespace android
