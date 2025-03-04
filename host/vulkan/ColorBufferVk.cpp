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

#include "ColorBufferVk.h"

#include "VkCommonOperations.h"

namespace gfxstream {
namespace vk {

/*static*/
std::unique_ptr<ColorBufferVk> ColorBufferVk::create(uint32_t handle, uint32_t width,
                                                     uint32_t height, GLenum format,
                                                     FrameworkFormat frameworkFormat,
                                                     bool vulkanOnly, uint32_t memoryProperty,
                                                     android::base::Stream* stream) {
    auto emulationVk = VkEmulation::get();
    if (!emulationVk->createVkColorBuffer(width, height, format, frameworkFormat, handle,
                                          vulkanOnly, memoryProperty)) {
        GL_LOG("Failed to create ColorBufferVk:%d", handle);
        return nullptr;
    }
    if (emulationVk->getFeatures().VulkanSnapshots.enabled && stream) {
        VkImageLayout currentLayout = static_cast<VkImageLayout>(stream->getBe32());
        emulationVk->setColorBufferCurrentLayout(handle, currentLayout);
    }
    return std::unique_ptr<ColorBufferVk>(new ColorBufferVk(handle));
}

void ColorBufferVk::onSave(android::base::Stream* stream) {
    auto emulationVk = VkEmulation::get();
    if (!emulationVk->getFeatures().VulkanSnapshots.enabled) {
        return;
    }
    stream->putBe32(static_cast<uint32_t>(emulationVk->getColorBufferCurrentLayout(mHandle)));
}

ColorBufferVk::ColorBufferVk(uint32_t handle) : mHandle(handle) {}

ColorBufferVk::~ColorBufferVk() {
    if (!VkEmulation::get()->teardownVkColorBuffer(mHandle)) {
        ERR("Failed to destroy ColorBufferVk:%d", mHandle);
    }
}

bool ColorBufferVk::readToBytes(std::vector<uint8_t>* outBytes) {
    return VkEmulation::get()->readColorBufferToBytes(mHandle, outBytes);
}

bool ColorBufferVk::readToBytes(uint32_t x, uint32_t y, uint32_t w, uint32_t h, void* outBytes,
                                uint64_t outBytesSize) {
    return VkEmulation::get()->readColorBufferToBytes(mHandle, x, y, w, h, outBytes, outBytesSize);
}

bool ColorBufferVk::updateFromBytes(const std::vector<uint8_t>& bytes) {
    return VkEmulation::get()->updateColorBufferFromBytes(mHandle, bytes);
}

bool ColorBufferVk::updateFromBytes(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                    const void* bytes) {
    return VkEmulation::get()->updateColorBufferFromBytes(mHandle, x, y, w, h, bytes);
}

std::unique_ptr<BorrowedImageInfo> ColorBufferVk::borrowForComposition(bool colorBufferIsTarget) {
    return VkEmulation::get()->borrowColorBufferForComposition(mHandle, colorBufferIsTarget);
}

std::unique_ptr<BorrowedImageInfo> ColorBufferVk::borrowForDisplay() {
    return VkEmulation::get()->borrowColorBufferForDisplay(mHandle);
}

std::optional<BlobDescriptorInfo> ColorBufferVk::exportBlob() {
    auto info = VkEmulation::get()->exportColorBufferMemory(mHandle);
    if (info) {
        return BlobDescriptorInfo{
            .descriptorInfo =
                {
#ifdef _WIN32
                    .descriptor = ManagedDescriptor(static_cast<DescriptorType>(
                        reinterpret_cast<void*>(info->handleInfo.handle))),
#else
                    .descriptor =
                        ManagedDescriptor(static_cast<DescriptorType>(info->handleInfo.handle)),
#endif
                    .streamHandleType = info->handleInfo.streamHandleType,
                },
            .caching = 0,
            .vulkanInfoOpt = std::nullopt,
        };
    } else {
        return std::nullopt;
    }
}

}  // namespace vk
}  // namespace gfxstream
