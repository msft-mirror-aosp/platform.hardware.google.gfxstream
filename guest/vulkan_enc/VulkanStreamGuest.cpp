// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "VulkanStreamGuest.h"

namespace gfxstream {
namespace vk {

VulkanStreamGuest::VulkanStreamGuest(IOStream *stream): mStream(stream) {
    unsetHandleMapping();
    mFeatureBits = ResourceTracker::get()->getStreamFeatures();
}

VulkanStreamGuest::~VulkanStreamGuest() = default;

bool VulkanStreamGuest::valid() {
    return true;
}

void VulkanStreamGuest::alloc(void** ptrAddr, size_t bytes) {
    if (!bytes) {
        *ptrAddr = nullptr;
        return;
    }

    *ptrAddr = mPool.alloc(bytes);
}

void VulkanStreamGuest::loadStringInPlace(char** forOutput) {
    size_t len = getBe32();

    alloc((void**)forOutput, len + 1);

    memset(*forOutput, 0x0, len + 1);

    if (len > 0) read(*forOutput, len);
}

void VulkanStreamGuest::loadStringArrayInPlace(char*** forOutput) {
    size_t count = getBe32();

    if (!count) {
        *forOutput = nullptr;
        return;
    }

    alloc((void**)forOutput, count * sizeof(char*));

    char **stringsForOutput = *forOutput;

    for (size_t i = 0; i < count; i++) {
        loadStringInPlace(stringsForOutput + i);
    }
}

void VulkanStreamGuest::loadStringInPlaceWithStreamPtr(char** forOutput, uint8_t** streamPtr) {
    uint32_t len;
    memcpy(&len, *streamPtr, sizeof(uint32_t));
    *streamPtr += sizeof(uint32_t);
    gfxstream::guest::Stream::fromBe32((uint8_t*)&len);

    alloc((void**)forOutput, len + 1);

    memset(*forOutput, 0x0, len + 1);

    if (len > 0) {
        memcpy(*forOutput, *streamPtr, len);
        *streamPtr += len;
    }
}

void VulkanStreamGuest::loadStringArrayInPlaceWithStreamPtr(char*** forOutput, uint8_t** streamPtr) {
 uint32_t count;
    memcpy(&count, *streamPtr, sizeof(uint32_t));
    *streamPtr += sizeof(uint32_t);
    gfxstream::guest::Stream::fromBe32((uint8_t*)&count);
    if (!count) {
        *forOutput = nullptr;
        return;
    }

    alloc((void**)forOutput, count * sizeof(char*));

    char **stringsForOutput = *forOutput;

    for (size_t i = 0; i < count; i++) {
        loadStringInPlaceWithStreamPtr(stringsForOutput + i, streamPtr);
    }
}


ssize_t VulkanStreamGuest::read(void *buffer, size_t size) {
    if (!mStream->readback(buffer, size)) {
        ALOGE("FATAL: Could not read back %zu bytes", size);
        abort();
    }
    return size;
}

ssize_t VulkanStreamGuest::write(const void *buffer, size_t size) {
    uint8_t* streamBuf = (uint8_t*)mStream->alloc(size);
    memcpy(streamBuf, buffer, size);
    return size;
}

void VulkanStreamGuest::writeLarge(const void* buffer, size_t size) {
    mStream->writeFullyAsync(buffer, size);
}

void VulkanStreamGuest::clearPool() {
    mPool.freeAll();
}

void VulkanStreamGuest::setHandleMapping(VulkanHandleMapping* mapping) {
    mCurrentHandleMapping = mapping;
}

void VulkanStreamGuest::unsetHandleMapping() {
    mCurrentHandleMapping = &mDefaultHandleMapping;
}

VulkanHandleMapping* VulkanStreamGuest::handleMapping() const {
    return mCurrentHandleMapping;
}

void VulkanStreamGuest::flush() {
    AEMU_SCOPED_TRACE("VulkanStreamGuest device write");
    mStream->flush();
}

uint32_t VulkanStreamGuest::getFeatureBits() const {
    return mFeatureBits;
}

void VulkanStreamGuest::incStreamRef() {
    mStream->incRef();
}

bool VulkanStreamGuest::decStreamRef() {
    return mStream->decRef();
}

uint8_t* VulkanStreamGuest::reserve(size_t size) {
    return (uint8_t*)mStream->alloc(size);
}

VulkanCountingStream::VulkanCountingStream() : VulkanStreamGuest(nullptr) { }
VulkanCountingStream::~VulkanCountingStream() = default;

ssize_t VulkanCountingStream::read(void*, size_t size) {
    m_read += size;
    return size;
}

ssize_t VulkanCountingStream::write(const void*, size_t size) {
    m_written += size;
    return size;
}

void VulkanCountingStream::rewind() {
    m_written = 0;
    m_read = 0;
}

}  // namespace vk
}  // namespace gfxstream
