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
#include <android/hardware/graphics/allocator/3.0/IAllocator.h>
#include <android/hardware/graphics/mapper/2.0/IMapper.h>
#include <android/hardware/graphics/mapper/3.0/IMapper.h>
#include <log/log.h>

#include "cbmanager.h"
#include "debug.h"

namespace android {
namespace {
using hardware::hidl_handle;
using hardware::hidl_vec;

namespace IMapper2ns = hardware::graphics::mapper::V2_0;
namespace IMapper3ns = hardware::graphics::mapper::V3_0;
namespace IAllocator2ns = hardware::graphics::allocator::V2_0;
namespace IAllocator3ns = hardware::graphics::allocator::V3_0;

class CbManagerHidlV3Impl : public CbManager::CbManagerImpl {
public:
    typedef CbManager::BufferUsage BufferUsage;
    typedef CbManager::PixelFormat PixelFormat;
    typedef CbManager::YCbCrLayout YCbCrLayout;
    typedef hardware::hidl_bitfield<BufferUsage> BufferUsageBits;

    CbManagerHidlV3Impl(sp<IMapper3ns::IMapper> mapper,
                        sp<IAllocator3ns::IAllocator> allocator)
      : mMapper(mapper), mAllocator(allocator) {}

    native_handle_t* allocateBuffer(int width, int height,
                                    PixelFormat format, BufferUsageBits usage) {
        using IMapper3ns::Error;
        using IMapper3ns::BufferDescriptor;

        IMapper3ns::IMapper::BufferDescriptorInfo descriptor_info;
        descriptor_info.width = width;
        descriptor_info.height = height;
        descriptor_info.layerCount = 1;
        descriptor_info.format =
            static_cast<hardware::graphics::common::V1_2::PixelFormat>(format);
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

        native_handle_t *buf = nullptr;
        mMapper->importBuffer(raw_handle, [&](const Error &_error,
                                              void *_buf) {
            hidl_err = _error;
            buf = static_cast<native_handle_t*>(_buf);
        });
        if (hidl_err != Error::NONE) {
            RETURN_ERROR(nullptr);
        }

        RETURN(buf);
    }

    void freeBuffer(const native_handle_t* _h) {
        using IMapper2ns::Error;

        native_handle_t* h = const_cast<native_handle_t*>(_h);

        mMapper->freeBuffer(h);
        native_handle_close(h);
        native_handle_delete(h);
    }

    int lockBuffer(native_handle_t& handle,
                   BufferUsageBits usage,
                   int left, int top, int width, int height,
                   void** vaddr) {
        using IMapper3ns::Error;

        Error hidl_err = Error::NONE;
        mMapper->lock(
            &handle,
            usage,
            { left, top, width, height },  // rect
            hidl_handle(),  // fence
            [&hidl_err, vaddr](const Error &_error,
                               void* _ptr,
                               int32_t /*bytesPerPixel*/,
                               int32_t /*bytesPerStride*/) {
                hidl_err = _error;
                if (_error == Error::NONE) {
                    *vaddr = _ptr;
                }
            });

        if (hidl_err == Error::NONE) {
            RETURN(0);
        } else {
            RETURN_ERROR(-1);
        }
    }

    int lockYCbCrBuffer(native_handle_t& handle,
                        BufferUsageBits usage,
                        int left, int top, int width, int height,
                        YCbCrLayout* ycbcr) {
        using IMapper3ns::Error;
        typedef IMapper3ns::YCbCrLayout YCbCrLayout3;

        Error hidl_err = Error::NONE;
        mMapper->lockYCbCr(
            &handle,
            usage,
            { left, top, width, height },  // rect
            hidl_handle(),  // fence
            [&hidl_err, ycbcr](const Error &_error,
                               const YCbCrLayout3 &_ycbcr) {
                hidl_err = _error;
                if (_error == Error::NONE) {
                    ycbcr->y = _ycbcr.y;
                    ycbcr->cb = _ycbcr.cb;
                    ycbcr->cr = _ycbcr.cr;
                    ycbcr->yStride = _ycbcr.yStride;
                    ycbcr->cStride = _ycbcr.cStride;
                    ycbcr->chromaStep = _ycbcr.chromaStep;
                }
            });

        if (hidl_err == Error::NONE) {
            RETURN(0);
        } else {
            RETURN_ERROR(-1);
        }
    }

    int unlockBuffer(native_handle_t& handle) {
        using IMapper3ns::Error;

        Error hidl_err = Error::NONE;
        int fence = -1;
        mMapper->unlock(
            &handle,
            [&hidl_err, &fence](const Error &_error,
                                const hidl_handle &_fence) {
                hidl_err = _error;
                (void)_fence;
            });

        if (hidl_err == Error::NONE) {
            RETURN(0);
        } else {
            RETURN_ERROR(-1);
        }
    }

private:
    const sp<IMapper3ns::IMapper> mMapper;
    const sp<IAllocator3ns::IAllocator> mAllocator;
};

class CbManagerHidlV2Impl : public CbManager::CbManagerImpl {
public:
    typedef CbManager::BufferUsage BufferUsage;
    typedef CbManager::PixelFormat PixelFormat;
    typedef CbManager::YCbCrLayout YCbCrLayout;
    typedef hardware::hidl_bitfield<BufferUsage> BufferUsageBits;

    CbManagerHidlV2Impl(sp<IMapper2ns::IMapper> mapper,
                        sp<IAllocator2ns::IAllocator> allocator)
      : mMapper(mapper), mAllocator(allocator) {}

    native_handle_t* allocateBuffer(int width, int height,
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

        native_handle_t *buf = nullptr;
        mMapper->importBuffer(raw_handle, [&](const Error &_error,
                                              void *_buf) {
            hidl_err = _error;
            buf = static_cast<native_handle_t*>(_buf);
        });
        if (hidl_err != Error::NONE) {
            RETURN_ERROR(nullptr);
        }

        RETURN(buf);
    }

    void freeBuffer(const native_handle_t* _h) {
        using IMapper2ns::Error;

        native_handle_t* h = const_cast<native_handle_t*>(_h);

        mMapper->freeBuffer(h);
        native_handle_close(h);
        native_handle_delete(h);
    }

    int lockBuffer(native_handle_t& handle,
                   BufferUsageBits usage,
                   int left, int top, int width, int height,
                   void** vaddr) {
        using IMapper2ns::Error;

        Error hidl_err = Error::NONE;
        mMapper->lock(
            &handle,
            usage,
            { left, top, width, height },  // rect
            hidl_handle(),  // fence
            [&hidl_err, vaddr](const Error &_error,
                               void* _ptr) {
                hidl_err = _error;
                if (_error == Error::NONE) {
                    *vaddr = _ptr;
                }
            });

        if (hidl_err == Error::NONE) {
            RETURN(0);
        } else {
            RETURN_ERROR(-1);
        }
    }

    int lockYCbCrBuffer(native_handle_t& handle,
                        BufferUsageBits usage,
                        int left, int top, int width, int height,
                        YCbCrLayout* ycbcr) {
        using IMapper2ns::Error;

        Error hidl_err = Error::NONE;
        mMapper->lockYCbCr(
            &handle,
            usage,
            { left, top, width, height },  // rect
            hidl_handle(),  // fence
            [&hidl_err, ycbcr](const Error &_error,
                               const YCbCrLayout &_ycbcr) {
                hidl_err = _error;
                if (_error == Error::NONE) {
                    *ycbcr = _ycbcr;
                }
            });

        if (hidl_err == Error::NONE) {
            RETURN(0);
        } else {
            RETURN_ERROR(-1);
        }
    }

    int unlockBuffer(native_handle_t& handle) {
        using IMapper2ns::Error;

        Error hidl_err = Error::NONE;
        int fence = -1;
        mMapper->unlock(
            &handle,
            [&hidl_err, &fence](const Error &_error,
                                const hidl_handle &_fence) {
                hidl_err = _error;
                (void)_fence;
            });

        if (hidl_err == Error::NONE) {
            RETURN(0);
        } else {
            RETURN_ERROR(-1);
        }
    }

private:
    const sp<IMapper2ns::IMapper> mMapper;
    const sp<IAllocator2ns::IAllocator> mAllocator;
};

std::unique_ptr<CbManager::CbManagerImpl> buildHidlImpl() {
    {
        sp<IMapper3ns::IMapper> mapper =
            IMapper3ns::IMapper::getService();
        if (!mapper) {
           ALOGW("%s:%d: no IMapper@3.0 implementation found", __func__, __LINE__);
        }

        sp<IAllocator3ns::IAllocator> allocator =
            IAllocator3ns::IAllocator::getService();
        if (!allocator) {
            ALOGW("%s:%d: no IAllocator@3.0 implementation found", __func__, __LINE__);
        }

        if (mapper && allocator) {
            return std::make_unique<CbManagerHidlV3Impl>(mapper, allocator);
        }
    }
    {
        sp<IMapper2ns::IMapper> mapper =
            IMapper2ns::IMapper::getService();
        if (!mapper) {
            ALOGW("%s:%d: no IMapper@2.0 implementation found", __func__, __LINE__);
        }

        sp<IAllocator2ns::IAllocator> allocator =
            IAllocator2ns::IAllocator::getService();
        if (!allocator) {
            ALOGW("%s:%d: no IAllocator@2.0 implementation found", __func__, __LINE__);
        }

        return std::make_unique<CbManagerHidlV2Impl>(mapper, allocator);
    }

    return nullptr;
}

}  // namespace

CbManager::CbManager() : mImpl(buildHidlImpl()) {}

}  // namespace android
