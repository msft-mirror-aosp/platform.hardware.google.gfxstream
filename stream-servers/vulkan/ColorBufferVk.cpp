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

/*static*/
std::unique_ptr<ColorBufferVk> ColorBufferVk::create(uint32_t handle, uint32_t width,
                                                     uint32_t height, GLenum format,
                                                     FrameworkFormat frameworkFormat,
                                                     bool vulkanOnly, uint32_t memoryProperty) {
    if (!goldfish_vk::setupVkColorBuffer(width, height, format, frameworkFormat, handle, vulkanOnly,
                                         memoryProperty)) {
        ERR("Failed to create ColorBufferVk:%d", handle);
        return nullptr;
    }

    return std::unique_ptr<ColorBufferVk>(new ColorBufferVk(handle));
}

ColorBufferVk::ColorBufferVk(uint32_t handle) : mHandle(handle) {}

ColorBufferVk::~ColorBufferVk() {
    if (!goldfish_vk::teardownVkColorBuffer(mHandle)) {
        ERR("Failed to destroy ColorBufferVk:%d", mHandle);
    }
}

}  // namespace gfxstream