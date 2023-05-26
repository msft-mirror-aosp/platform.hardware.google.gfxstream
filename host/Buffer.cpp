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

#include "Buffer.h"

#include "gl/EmulationGl.h"
#include "vulkan/BufferVk.h"
#include "vulkan/VkCommonOperations.h"

namespace gfxstream {

using android::base::ManagedDescriptor;
using emugl::ABORT_REASON_OTHER;
using emugl::FatalError;

Buffer::Buffer(HandleType handle, uint64_t size) : mHandle(handle), mSize(size) {}

/*static*/
std::shared_ptr<Buffer> Buffer::create(gl::EmulationGl* emulationGl, vk::VkEmulation* emulationVk,
                                       uint64_t size, HandleType handle) {
    std::shared_ptr<Buffer> buffer(new Buffer(handle, size));

    if (emulationGl) {
        buffer->mBufferGl = emulationGl->createBuffer(size, handle);
        if (!buffer->mBufferGl) {
            ERR("Failed to initialize BufferGl.");
            return nullptr;
        }
    }

    if (emulationVk && emulationVk->live) {
        const bool vulkanOnly = emulationGl == nullptr;

        buffer->mBufferVk = vk::BufferVk::create(handle, size, vulkanOnly);
        if (!buffer->mBufferVk) {
            ERR("Failed to initialize BufferVk.");
            return nullptr;
        }

        if (!vulkanOnly) {
            if (!buffer->mBufferGl) {
                GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "Missing BufferGl?";
            }

            // TODO: external memory sharing.
        }
    }

    return buffer;
}

/*static*/
std::shared_ptr<Buffer> Buffer::onLoad(gl::EmulationGl* emulationGl, vk::VkEmulation*,
                                       android::base::Stream* stream) {
    const auto handle = static_cast<HandleType>(stream->getBe32());
    const auto size = static_cast<uint64_t>(stream->getBe64());

    std::shared_ptr<Buffer> buffer(new Buffer(handle, size));

    if (emulationGl) {
        buffer->mBufferGl = emulationGl->loadBuffer(stream);
        if (!buffer->mBufferGl) {
            ERR("Failed to load BufferGl.");
            return nullptr;
        }
    }

    buffer->mNeedRestore = true;

    return buffer;
}

void Buffer::onSave(android::base::Stream* stream) {
    stream->putBe32(mHandle);
    stream->putBe64(mSize);

    if (mBufferGl) {
        mBufferGl->onSave(stream);
    }
}

void Buffer::restore() {}

void Buffer::readToBytes(uint64_t offset, uint64_t size, void* outBytes) {
    touch();

    if (mBufferGl) {
        mBufferGl->read(offset, size, outBytes);
        return;
    }
    if (mBufferVk) {
        mBufferVk->readToBytes(offset, size, outBytes);
        return;
    }

    GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "No Buffer impl?";
}

bool Buffer::updateFromBytes(uint64_t offset, uint64_t size, const void* bytes) {
    touch();

    if (mBufferGl) {
        mBufferGl->subUpdate(offset, size, bytes);
        return true;
    }
    if (mBufferVk) {
        return mBufferVk->updateFromBytes(offset, size, bytes);
    }

    GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << "No Buffer impl?";
    return false;
}

}  // namespace gfxstream
