// Copyright 2022 The Android Open Source Project
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

#include "CompressedImageInfo.h"

#include "aemu/base/ArraySize.h"
#include "stream-servers/vulkan/VkFormatUtils.h"
#include "stream-servers/vulkan/emulated_textures/shaders/DecompressionShaders.h"

namespace goldfish_vk {

namespace {

using android::base::arraySize;

struct Etc2PushConstant {
    uint32_t compFormat;
    uint32_t baseLayer;
};

struct AstcPushConstant {
    uint32_t blockSize[2];
    uint32_t compFormat;
    uint32_t baseLayer;
    uint32_t sRGB;
    uint32_t smallBlock;
};

struct ShaderData {
    const uint32_t* code;
    const size_t size;
};

struct ShaderGroup {
    ShaderData shader1D;
    ShaderData shader2D;
    ShaderData shader3D;
};

#define DECLARE_SHADER_GROUP(Format)                                      \
    constexpr ShaderGroup kShader##Format {                               \
        .shader1D = {.code = decompression_shaders::Format##_1D,          \
                     .size = sizeof(decompression_shaders::Format##_1D)}, \
        .shader2D = {.code = decompression_shaders::Format##_2D,          \
                     .size = sizeof(decompression_shaders::Format##_2D)}, \
        .shader3D = {.code = decompression_shaders::Format##_3D,          \
                     .size = sizeof(decompression_shaders::Format##_3D)}, \
    }

DECLARE_SHADER_GROUP(Astc);
DECLARE_SHADER_GROUP(EacR11Snorm);
DECLARE_SHADER_GROUP(EacR11Unorm);
DECLARE_SHADER_GROUP(EacRG11Snorm);
DECLARE_SHADER_GROUP(EacRG11Unorm);
DECLARE_SHADER_GROUP(Etc2RGB8);
DECLARE_SHADER_GROUP(Etc2RGBA8);

#undef DECLARE_SHADER_GROUP

const ShaderGroup* getShaderGroup(VkFormat format) {
    switch (format) {
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            return &kShaderAstc;

        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
            return &kShaderEacR11Snorm;

        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
            return &kShaderEacR11Unorm;

        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
            return &kShaderEacRG11Snorm;

        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
            return &kShaderEacRG11Unorm;

        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
            return &kShaderEtc2RGB8;

        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
            return &kShaderEtc2RGBA8;

        default:
            return nullptr;
    }
}

const ShaderData* getDecompressionShader(VkFormat format, VkImageType imageType) {
    const ShaderGroup* group = getShaderGroup(format);
    if (!group) return nullptr;

    switch (imageType) {
        case VK_IMAGE_TYPE_1D:
            return &group->shader1D;
        case VK_IMAGE_TYPE_2D:
            return &group->shader2D;
        case VK_IMAGE_TYPE_3D:
            return &group->shader3D;
        default:
            return nullptr;
    }
}

VkImageView createDefaultImageView(goldfish_vk::VulkanDispatch* vk, VkDevice device, VkImage image,
                                   VkFormat format, VkImageType imageType, uint32_t mipLevel,
                                   uint32_t layerCount) {
    VkImageViewCreateInfo imageViewInfo = {};
    imageViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewInfo.image = image;
    switch (imageType) {
        case VK_IMAGE_TYPE_1D:
            imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
            break;
        case VK_IMAGE_TYPE_2D:
            imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            break;
        case VK_IMAGE_TYPE_3D:
            imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
            break;
        default:
            imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            break;
    }
    imageViewInfo.format = format;
    imageViewInfo.components.r = VK_COMPONENT_SWIZZLE_R;
    imageViewInfo.components.g = VK_COMPONENT_SWIZZLE_G;
    imageViewInfo.components.b = VK_COMPONENT_SWIZZLE_B;
    imageViewInfo.components.a = VK_COMPONENT_SWIZZLE_A;
    imageViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageViewInfo.subresourceRange.baseMipLevel = mipLevel;
    imageViewInfo.subresourceRange.levelCount = 1;
    imageViewInfo.subresourceRange.baseArrayLayer = 0;
    imageViewInfo.subresourceRange.layerCount = layerCount;
    VkImageView imageView;
    if (VK_SUCCESS != vk->vkCreateImageView(device, &imageViewInfo, nullptr, &imageView)) {
        fprintf(stderr, "Warning: %s %s:%d failure\n", __func__, __FILE__, __LINE__);
        return 0;
    }
    return imageView;
}

std::pair<uint32_t, uint32_t> getBlockSize(VkFormat format) {
    switch (format) {
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
            return {4, 4};
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
            return {4, 4};
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
            return {5, 4};
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
            return {5, 5};
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
            return {6, 5};
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
            return {6, 6};
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
            return {8, 5};
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
            return {8, 6};
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
            return {8, 8};
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
            return {10, 5};
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
            return {10, 6};
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
            return {10, 8};
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
            return {10, 10};
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
            return {12, 10};
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            return {12, 12};
        default:
            return {1, 1};
    }
}

}  // namespace

CompressedImageInfo::CompressedImageInfo(const VkImageCreateInfo& createInfo)
    : compFormat(createInfo.format),
      decompFormat(getDecompFormat(compFormat)),
      sizeCompFormat(getSizeCompFormat(compFormat)),
      isCompressed((decompFormat != compFormat)),
      sizeCompImgCreateInfo(createInfo),
      imageType(createInfo.imageType),
      extent(createInfo.extent),
      layerCount(createInfo.arrayLayers),
      mipLevels(createInfo.mipLevels) {
    auto blockSize = getBlockSize(compFormat);
    blockWidth = blockSize.first;
    blockHeight = blockSize.second;

    sizeCompImgCreateInfo.format = sizeCompFormat;
    sizeCompImgCreateInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    sizeCompImgCreateInfo.flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT_KHR;
    sizeCompImgCreateInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    sizeCompImgCreateInfo.extent.width =
        (sizeCompImgCreateInfo.extent.width + blockWidth - 1) / blockWidth;
    sizeCompImgCreateInfo.extent.height =
        (sizeCompImgCreateInfo.extent.height + blockHeight - 1) / blockHeight;
    sizeCompImgCreateInfo.mipLevels = 1;
    if (createInfo.queueFamilyIndexCount) {
        sizeCompImgQueueFamilyIndices.assign(
            createInfo.pQueueFamilyIndices,
            createInfo.pQueueFamilyIndices + createInfo.queueFamilyIndexCount);
        sizeCompImgCreateInfo.pQueueFamilyIndices = sizeCompImgQueueFamilyIndices.data();
    }
}

// static
VkFormat CompressedImageInfo::getDecompFormat(VkFormat compFmt) {
    switch (compFmt) {
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
            return VK_FORMAT_R16_UNORM;
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
            return VK_FORMAT_R16_SNORM;
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
            return VK_FORMAT_R16G16_UNORM;
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
            return VK_FORMAT_R16G16_SNORM;
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            return VK_FORMAT_R8G8B8A8_SRGB;
        default:
            return compFmt;
    }
}

// static
VkFormat CompressedImageInfo::getSizeCompFormat(VkFormat compFmt) {
    switch (compFmt) {
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
            return VK_FORMAT_R16G16B16A16_UINT;
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
            return VK_FORMAT_R32G32B32A32_UINT;
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
            return VK_FORMAT_R32G32_UINT;
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
            return VK_FORMAT_R32G32B32A32_UINT;
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            return VK_FORMAT_R32G32B32A32_UINT;
        default:
            return compFmt;
    }
}

// static
bool CompressedImageInfo::isEtc2(VkFormat format) {
    switch (format) {
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
            return true;
        default:
            return false;
    }
}

// static
bool CompressedImageInfo::isAstc(VkFormat format) {
    switch (format) {
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            return true;
        default:
            return false;
    }
}

// static
bool CompressedImageInfo::needEmulatedAlpha(VkFormat format) {
    switch (format) {
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
            return true;
        default:
            return false;
    }
}

bool CompressedImageInfo::isEtc2() const { return isEtc2(compFormat); }

bool CompressedImageInfo::isAstc() const { return isAstc(compFormat); }

void CompressedImageInfo::createSizeCompImages(goldfish_vk::VulkanDispatch* vk) {
    if (sizeCompImgs.size() > 0) {
        return;
    }
    sizeCompImgs.resize(mipLevels);
    VkImageCreateInfo imageInfo = sizeCompImgCreateInfo;
    imageInfo.mipLevels = 1;
    for (uint32_t i = 0; i < mipLevels; i++) {
        imageInfo.extent.width = sizeCompMipmapWidth(i);
        imageInfo.extent.height = sizeCompMipmapHeight(i);
        imageInfo.extent.depth = sizeCompMipmapDepth(i);
        vk->vkCreateImage(device, &imageInfo, nullptr, sizeCompImgs.data() + i);
    }

    std::vector<VkDeviceSize> memSizes(mipLevels);
    VkDeviceSize decompImageSize = 0;
    {
        VkMemoryRequirements memRequirements;
        vk->vkGetImageMemoryRequirements(device, decompImg, &memRequirements);
        alignment = std::max(alignment, memRequirements.alignment);
        decompImageSize = memRequirements.size;
    }
    for (size_t i = 0; i < mipLevels; i++) {
        VkMemoryRequirements memRequirements;
        vk->vkGetImageMemoryRequirements(device, sizeCompImgs[i], &memRequirements);
        alignment = std::max(alignment, memRequirements.alignment);
        memSizes[i] = memRequirements.size;
    }
    memoryOffsets.resize(mipLevels + 1);
    {
        VkDeviceSize alignedSize = decompImageSize;
        if (alignment != 0) {
            alignedSize = (alignedSize + alignment - 1) / alignment * alignment;
        }
        memoryOffsets[0] = alignedSize;
    }
    for (size_t i = 0; i < sizeCompImgs.size(); i++) {
        VkDeviceSize alignedSize = memSizes[i];
        if (alignment != 0) {
            alignedSize = (alignedSize + alignment - 1) / alignment * alignment;
        }
        memoryOffsets[i + 1] = memoryOffsets[i] + alignedSize;
    }
}

VkBufferImageCopy CompressedImageInfo::getSizeCompBufferImageCopy(
    const VkBufferImageCopy& origRegion) const {
    VkBufferImageCopy region = origRegion;
    uint32_t mipLevel = region.imageSubresource.mipLevel;
    region.imageSubresource.mipLevel = 0;
    region.bufferRowLength /= blockWidth;
    region.bufferImageHeight /= blockHeight;
    region.imageOffset.x /= blockWidth;
    region.imageOffset.y /= blockHeight;
    region.imageExtent.width = sizeCompWidth(region.imageExtent.width, mipLevel);
    region.imageExtent.height = sizeCompHeight(region.imageExtent.height, mipLevel);

    return region;
}

// static
VkImageCopy CompressedImageInfo::getSizeCompImageCopy(const VkImageCopy& origRegion,
                                                      const CompressedImageInfo& srcImg,
                                                      const CompressedImageInfo& dstImg,
                                                      bool needEmulatedSrc, bool needEmulatedDst) {
    VkImageCopy region = origRegion;
    if (needEmulatedSrc) {
        uint32_t mipLevel = region.srcSubresource.mipLevel;
        region.srcSubresource.mipLevel = 0;
        region.srcOffset.x /= srcImg.blockWidth;
        region.srcOffset.y /= srcImg.blockHeight;
        region.extent.width = srcImg.sizeCompWidth(region.extent.width, mipLevel);
        region.extent.height = srcImg.sizeCompHeight(region.extent.height, mipLevel);
    }
    if (needEmulatedDst) {
        region.dstSubresource.mipLevel = 0;
        region.dstOffset.x /= dstImg.blockWidth;
        region.dstOffset.y /= dstImg.blockHeight;
    }
    return region;
}

#define _RETURN_ON_FAILURE(cmd)                                                                    \
    {                                                                                              \
        VkResult result = cmd;                                                                     \
        if (VK_SUCCESS != result) {                                                                \
            fprintf(stderr, "Warning: %s %s:%d vulkan failure %d\n", __func__, __FILE__, __LINE__, \
                    result);                                                                       \
            return (result);                                                                       \
        }                                                                                          \
    }

VkResult CompressedImageInfo::initDecomp(goldfish_vk::VulkanDispatch* vk, VkDevice device,
                                         VkImage image) {
    if (decompPipeline != 0) {
        return VK_SUCCESS;
    }
    // TODO: release resources on failure

    const ShaderData* shader = getDecompressionShader(compFormat, imageType);
    if (!shader) {
        WARN("No decompression shader found for format %s and img type %s",
             string_VkFormat(compFormat), string_VkImageType(imageType));
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VkShaderModuleCreateInfo shaderInfo = {};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = shader->size;
    shaderInfo.pCode = shader->code;
    _RETURN_ON_FAILURE(vk->vkCreateShaderModule(device, &shaderInfo, nullptr, &decompShader));

    VkDescriptorSetLayoutBinding dsLayoutBindings[] = {
        {
            0,                                 // bindings
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  // descriptorType
            1,                                 // descriptorCount
            VK_SHADER_STAGE_COMPUTE_BIT,       // stageFlags
            0,                                 // pImmutableSamplers
        },
        {
            1,                                 // bindings
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  // descriptorType
            1,                                 // descriptorCount
            VK_SHADER_STAGE_COMPUTE_BIT,       // stageFlags
            0,                                 // pImmutableSamplers
        },
    };
    VkDescriptorSetLayoutCreateInfo dsLayoutInfo = {};
    dsLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsLayoutInfo.bindingCount = sizeof(dsLayoutBindings) / sizeof(VkDescriptorSetLayoutBinding);
    dsLayoutInfo.pBindings = dsLayoutBindings;
    _RETURN_ON_FAILURE(vk->vkCreateDescriptorSetLayout(device, &dsLayoutInfo, nullptr,
                                                       &decompDescriptorSetLayout));

    VkDescriptorPoolSize poolSize[1] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * mipLevels},
    };
    VkDescriptorPoolCreateInfo dsPoolInfo = {};
    dsPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dsPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dsPoolInfo.maxSets = mipLevels;
    dsPoolInfo.poolSizeCount = 1;
    dsPoolInfo.pPoolSizes = poolSize;
    _RETURN_ON_FAILURE(
        vk->vkCreateDescriptorPool(device, &dsPoolInfo, nullptr, &decompDescriptorPool));
    std::vector<VkDescriptorSetLayout> layouts(mipLevels, decompDescriptorSetLayout);

    VkDescriptorSetAllocateInfo dsInfo = {};
    dsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsInfo.descriptorPool = decompDescriptorPool;
    dsInfo.descriptorSetCount = mipLevels;
    dsInfo.pSetLayouts = layouts.data();
    decompDescriptorSets.resize(mipLevels);
    _RETURN_ON_FAILURE(vk->vkAllocateDescriptorSets(device, &dsInfo, decompDescriptorSets.data()));

    VkPushConstantRange pushConstant = {};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    if (isEtc2()) {
        pushConstant.size = sizeof(Etc2PushConstant);
    } else if (isAstc()) {
        pushConstant.size = sizeof(AstcPushConstant);
    }

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &decompDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstant;
    _RETURN_ON_FAILURE(
        vk->vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &decompPipelineLayout));

    VkComputePipelineCreateInfo computePipelineInfo = {};
    computePipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    computePipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    computePipelineInfo.stage.module = decompShader;
    computePipelineInfo.stage.pName = "main";
    computePipelineInfo.layout = decompPipelineLayout;
    _RETURN_ON_FAILURE(
        vk->vkCreateComputePipelines(device, 0, 1, &computePipelineInfo, nullptr, &decompPipeline));

    VkFormat intermediateFormat;
    switch (compFormat) {
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
            intermediateFormat = VK_FORMAT_R8G8B8A8_UINT;
            break;
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
            intermediateFormat = decompFormat;
            break;
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            intermediateFormat = VK_FORMAT_R8G8B8A8_UINT;
            break;
        default:
            intermediateFormat = decompFormat;
            break;  // should not happen
    }

    sizeCompImageViews.resize(mipLevels);
    decompImageViews.resize(mipLevels);
    VkDescriptorImageInfo sizeCompDescriptorImageInfo[1] = {{}};
    sizeCompDescriptorImageInfo[0].sampler = 0;
    sizeCompDescriptorImageInfo[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo decompDescriptorImageInfo[1] = {{}};
    decompDescriptorImageInfo[0].sampler = 0;
    decompDescriptorImageInfo[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writeDescriptorSets[2] = {{}, {}};
    writeDescriptorSets[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDescriptorSets[0].dstBinding = 0;
    writeDescriptorSets[0].descriptorCount = 1;
    writeDescriptorSets[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writeDescriptorSets[0].pImageInfo = sizeCompDescriptorImageInfo;

    writeDescriptorSets[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDescriptorSets[1].dstBinding = 1;
    writeDescriptorSets[1].descriptorCount = 1;
    writeDescriptorSets[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writeDescriptorSets[1].pImageInfo = decompDescriptorImageInfo;

    for (uint32_t i = 0; i < mipLevels; i++) {
        sizeCompImageViews[i] = createDefaultImageView(vk, device, sizeCompImgs[i], sizeCompFormat,
                                                       imageType, 0, layerCount);
        decompImageViews[i] = createDefaultImageView(vk, device, decompImg, intermediateFormat,
                                                     imageType, i, layerCount);
        sizeCompDescriptorImageInfo[0].imageView = sizeCompImageViews[i];
        decompDescriptorImageInfo[0].imageView = decompImageViews[i];
        writeDescriptorSets[0].dstSet = decompDescriptorSets[i];
        writeDescriptorSets[1].dstSet = decompDescriptorSets[i];
        vk->vkUpdateDescriptorSets(device, 2, writeDescriptorSets, 0, nullptr);
    }
    return VK_SUCCESS;
}

void CompressedImageInfo::cmdDecompress(goldfish_vk::VulkanDispatch* vk,
                                        VkCommandBuffer commandBuffer,
                                        VkPipelineStageFlags dstStageMask, VkImageLayout newLayout,
                                        VkAccessFlags dstAccessMask, uint32_t baseMipLevel,
                                        uint32_t levelCount, uint32_t baseLayer,
                                        uint32_t _layerCount) {
    vk->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, decompPipeline);
    int dispatchZ = _layerCount;

    if (isEtc2()) {
        Etc2PushConstant pushConstant = {(uint32_t)compFormat, baseLayer};
        if (extent.depth > 1) {
            // 3D texture
            pushConstant.baseLayer = 0;
            dispatchZ = extent.depth;
        }
        vk->vkCmdPushConstants(commandBuffer, decompPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(pushConstant), &pushConstant);
    } else if (isAstc()) {
        uint32_t srgb = false;
        uint32_t smallBlock = false;
        switch (compFormat) {
            case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
            case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
            case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
            case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
            case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
            case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
            case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
            case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
            case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
            case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
            case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
            case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
            case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
            case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
                srgb = true;
                break;
            default:
                break;
        }
        switch (compFormat) {
            case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
            case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
            case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
            case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
            case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
            case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
            case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
            case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
                smallBlock = true;
                break;
            default:
                break;
        }
        AstcPushConstant pushConstant = {
            {blockWidth, blockHeight}, (uint32_t)compFormat, baseLayer, srgb, smallBlock,
        };
        if (extent.depth > 1) {
            // 3D texture
            pushConstant.baseLayer = 0;
            dispatchZ = extent.depth;
        }
        vk->vkCmdPushConstants(commandBuffer, decompPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(pushConstant), &pushConstant);
    }
    for (uint32_t i = baseMipLevel; i < baseMipLevel + levelCount; i++) {
        vk->vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    decompPipelineLayout, 0, 1, decompDescriptorSets.data() + i, 0,
                                    nullptr);

        vk->vkCmdDispatch(commandBuffer, sizeCompMipmapWidth(i), sizeCompMipmapHeight(i),
                          dispatchZ);
    }
}

uint32_t CompressedImageInfo::mipmapWidth(uint32_t level) const {
    return std::max<uint32_t>(extent.width >> level, 1);
}

uint32_t CompressedImageInfo::mipmapHeight(uint32_t level) const {
    return std::max<uint32_t>(extent.height >> level, 1);
}

uint32_t CompressedImageInfo::mipmapDepth(uint32_t level) const {
    return std::max<uint32_t>(extent.depth >> level, 1);
}

uint32_t CompressedImageInfo::sizeCompMipmapWidth(uint32_t level) const {
    return (mipmapWidth(level) + blockWidth - 1) / blockWidth;
}

uint32_t CompressedImageInfo::sizeCompMipmapHeight(uint32_t level) const {
    if (imageType != VK_IMAGE_TYPE_1D) {
        return (mipmapHeight(level) + blockHeight - 1) / blockHeight;
    } else {
        return 1;
    }
}

uint32_t CompressedImageInfo::sizeCompMipmapDepth(uint32_t level) const {
    return mipmapDepth(level);
}

uint32_t CompressedImageInfo::sizeCompWidth(uint32_t width, uint32_t mipLevel) const {
    return std::min((width + blockWidth - 1) / blockWidth, sizeCompMipmapWidth(mipLevel));
}

uint32_t CompressedImageInfo::sizeCompHeight(uint32_t height, uint32_t mipLevel) const {
    return std::min((height + blockHeight - 1) / blockHeight, sizeCompMipmapHeight(mipLevel));
}

}  // namespace goldfish_vk
