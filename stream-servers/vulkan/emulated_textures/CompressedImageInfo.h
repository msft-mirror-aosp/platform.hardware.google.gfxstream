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

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "stream-servers/vulkan/emulated_textures/AstcTexture.h"
#include "vulkan/cereal/common/goldfish_vk_dispatch.h"
#include "vulkan/vulkan.h"

namespace goldfish_vk {

struct CompressedImageInfo {
    VkFormat compFormat = VK_FORMAT_UNDEFINED;      // The compressed format
    VkFormat decompFormat = VK_FORMAT_UNDEFINED;    // Decompressed format
    VkFormat sizeCompFormat = VK_FORMAT_UNDEFINED;  // Size compatible format
    bool isCompressed = false;
    VkDevice device = VK_NULL_HANDLE;
    VkImageCreateInfo sizeCompImgCreateInfo;
    VkImageType imageType;
    std::vector<uint32_t> sizeCompImgQueueFamilyIndices;
    VkDeviceSize alignment = 0;
    std::vector<VkDeviceSize> memoryOffsets = {};
    std::vector<VkImage> sizeCompImgs;  // Size compatible images
    VkImage decompImg = 0;              // Decompressed image
    VkExtent3D extent = {};
    uint32_t blockWidth = 1;
    uint32_t blockHeight = 1;
    uint32_t layerCount = 1;
    uint32_t mipLevels = 1;
    std::unique_ptr<AstcTexture> astcTexture = nullptr;
    VkDescriptorSetLayout decompDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool decompDescriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> decompDescriptorSets = {};
    VkShaderModule decompShader = VK_NULL_HANDLE;
    VkPipelineLayout decompPipelineLayout = VK_NULL_HANDLE;
    VkPipeline decompPipeline = VK_NULL_HANDLE;
    std::vector<VkImageView> sizeCompImageViews = {};
    std::vector<VkImageView> decompImageViews = {};

    CompressedImageInfo() = default;
    explicit CompressedImageInfo(const VkImageCreateInfo& createInfo);

    static VkFormat getDecompFormat(VkFormat compFmt);
    static VkFormat getSizeCompFormat(VkFormat compFmt);
    static bool isEtc2(VkFormat format);
    static bool isAstc(VkFormat format);
    static bool needEmulatedAlpha(VkFormat format);
    static VkImageCopy getSizeCompImageCopy(const VkImageCopy& origRegion,
                                            const CompressedImageInfo& srcImg,
                                            const CompressedImageInfo& dstImg, bool needEmulatedSrc,
                                            bool needEmulatedDst);

    bool isEtc2() const;
    bool isAstc() const;

    void createSizeCompImages(goldfish_vk::VulkanDispatch* vk);

    VkBufferImageCopy getSizeCompBufferImageCopy(const VkBufferImageCopy& origRegion) const;

    VkResult initDecomp(goldfish_vk::VulkanDispatch* vk, VkDevice device, VkImage image);

    void cmdDecompress(goldfish_vk::VulkanDispatch* vk, VkCommandBuffer commandBuffer,
                       VkPipelineStageFlags dstStageMask, VkImageLayout newLayout,
                       VkAccessFlags dstAccessMask, uint32_t baseMipLevel, uint32_t levelCount,
                       uint32_t baseLayer, uint32_t _layerCount);

   private:
    uint32_t mipmapWidth(uint32_t level) const;
    uint32_t mipmapHeight(uint32_t level) const;
    uint32_t mipmapDepth(uint32_t level) const;
    uint32_t sizeCompMipmapWidth(uint32_t level) const;
    uint32_t sizeCompMipmapHeight(uint32_t level) const;
    uint32_t sizeCompMipmapDepth(uint32_t level) const;
    uint32_t sizeCompWidth(uint32_t width, uint32_t mipLevel) const;
    uint32_t sizeCompHeight(uint32_t height, uint32_t mipLevel) const;
};

}  // namespace goldfish_vk
