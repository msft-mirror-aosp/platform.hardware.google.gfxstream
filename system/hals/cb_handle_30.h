/*
* Copyright 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#ifndef SYSTEM_HALS_CB_HANDLE_30_H
#define SYSTEM_HALS_CB_HANDLE_30_H

#include <gralloc_cb_bp.h>
#include "goldfish_address_space.h"

const uint32_t CB_HANDLE_MAGIC_30 = CB_HANDLE_MAGIC_BASE | 0x2;

struct cb_handle_30_t : public cb_handle_t {
    cb_handle_30_t(int p_bufferFd,
                   QEMU_PIPE_HANDLE p_hostHandleRefCountFd,
                   uint32_t p_hostHandle,
                   uint32_t p_usage,
                   uint32_t p_width,
                   uint32_t p_height,
                   uint32_t p_format,
                   uint32_t p_glFormat,
                   uint32_t p_glType,
                   uint32_t p_bufSize,
                   void* p_bufPtr,
                   uint32_t p_mmapedSize,
                   uint64_t p_mmapedOffset,
                   uint32_t p_bytesPerPixel,
                   uint32_t p_stride)
            : cb_handle_t(p_bufferFd,
                          p_hostHandleRefCountFd,
                          CB_HANDLE_MAGIC_30,
                          p_hostHandle,
                          p_format,
                          p_stride,
                          p_bufSize,
                          p_mmapedOffset),
              usage(p_usage),
              width(p_width),
              height(p_height),
              glFormat(p_glFormat),
              glType(p_glType),
              bytesPerPixel(p_bytesPerPixel),
              mmapedSize(p_mmapedSize),
              locked(0),
              lockedUsage(0),
              lockedLeft(0),
              lockedTop(0),
              lockedWidth(0),
              lockedHeight(0) {
        numInts = CB_HANDLE_NUM_INTS(numFds);
        setBufferPtr(p_bufPtr);
    }

    bool isValid() const { return (version == sizeof(native_handle_t)) && (magic == CB_HANDLE_MAGIC_30); }

    void* getBufferPtr() const {
        const uint64_t addr = (uint64_t(bufferPtrHi) << 32) | bufferPtrLo;
        return reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
    }

    void setBufferPtr(void* ptr) {
        const uint64_t addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
        bufferPtrLo = uint32_t(addr);
        bufferPtrHi = uint32_t(addr >> 32);
    }

    static cb_handle_30_t* from(void* p) {
        if (!p) { return nullptr; }
        cb_handle_30_t* cb = static_cast<cb_handle_30_t*>(p);
        return cb->isValid() ? cb : nullptr;
    }

    static const cb_handle_30_t* from(const void* p) {
        return from(const_cast<void*>(p));
    }

    static cb_handle_30_t* from_unconst(const void* p) {
        return from(const_cast<void*>(p));
    }

    uint32_t usage;         // usage bits the buffer was created with
    uint32_t width;         // buffer width
    uint32_t height;        // buffer height
    uint32_t glFormat;      // OpenGL format enum used for host h/w color buffer
    uint32_t glType;        // OpenGL type enum used when uploading to host
    uint32_t bytesPerPixel;
    uint32_t mmapedSize;    // real allocation side
    uint32_t bufferPtrLo;
    uint32_t bufferPtrHi;
    uint32_t locked;
    uint32_t lockedUsage;
    uint32_t lockedLeft;    // region of buffer locked for s/w write
    uint32_t lockedTop;
    uint32_t lockedWidth;
    uint32_t lockedHeight;
};

#endif // SYSTEM_HALS_CB_HANDLE_30_H
