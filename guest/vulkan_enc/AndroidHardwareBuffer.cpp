/// Copyright (C) 2019 The Android Open Source Project
// Copyright (C) 2019 Google Inc.
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
#include "AndroidHardwareBuffer.h"

#if !defined(HOST_BUILD)
#if defined(__ANDROID__) || defined(__linux__)
#include <drm_fourcc.h>
#define DRM_FORMAT_YVU420_ANDROID fourcc_code('9', '9', '9', '7')
#endif
#endif

#include "../OpenglSystemCommon/HostConnection.h"

#include "vk_format_info.h"
#include "vk_util.h"
#include <assert.h>

namespace gfxstream {
namespace vk {

// From Intel ANV implementation.
/* Construct ahw usage mask from image usage bits, see
 * 'AHardwareBuffer Usage Equivalence' in Vulkan spec.
 */
uint64_t
getAndroidHardwareBufferUsageFromVkUsage(const VkImageCreateFlags vk_create,
                                 const VkImageUsageFlags vk_usage)
{
   uint64_t ahw_usage = 0;

   if (vk_usage & VK_IMAGE_USAGE_SAMPLED_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

   if (vk_usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

   if (vk_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;

   if (vk_create & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP;

   if (vk_create & VK_IMAGE_CREATE_PROTECTED_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT;

   /* No usage bits set - set at least one GPU usage. */
   if (ahw_usage == 0)
      ahw_usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

   return ahw_usage;
}

void updateMemoryTypeBits(uint32_t* memoryTypeBits, uint32_t colorBufferMemoryIndex) {
   *memoryTypeBits = 1u << colorBufferMemoryIndex;
}

VkResult getAndroidHardwareBufferPropertiesANDROID(
    Gralloc* grallocHelper,
    const AHardwareBuffer* buffer,
    VkAndroidHardwareBufferPropertiesANDROID* pProperties) {

    const native_handle_t *handle =
       AHardwareBuffer_getNativeHandle(buffer);

    VkAndroidHardwareBufferFormatPropertiesANDROID* ahbFormatProps =
        vk_find_struct<VkAndroidHardwareBufferFormatPropertiesANDROID>(pProperties);

    if (ahbFormatProps) {
        AHardwareBuffer_Desc desc;
        AHardwareBuffer_describe(buffer, &desc);

       const uint64_t gpu_usage =
          AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
          AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
          AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;

        if (!(desc.usage & (gpu_usage))) {
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        switch(desc.format) {
            case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
                  ahbFormatProps->format = VK_FORMAT_R8G8B8A8_UNORM;
                  break;
            case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
                  ahbFormatProps->format = VK_FORMAT_R8G8B8A8_UNORM;
                  break;
            case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
                  ahbFormatProps->format = VK_FORMAT_R8G8B8_UNORM;
                  break;
            case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
                  ahbFormatProps->format = VK_FORMAT_R5G6B5_UNORM_PACK16;
                  break;
            case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
                  ahbFormatProps->format = VK_FORMAT_R16G16B16A16_SFLOAT;
                  break;
            case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
                  ahbFormatProps->format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
                  break;
            case AHARDWAREBUFFER_FORMAT_D16_UNORM:
                  ahbFormatProps->format = VK_FORMAT_D16_UNORM;
                  break;
            case AHARDWAREBUFFER_FORMAT_D24_UNORM:
                  ahbFormatProps->format = VK_FORMAT_X8_D24_UNORM_PACK32;
                  break;
            case AHARDWAREBUFFER_FORMAT_D24_UNORM_S8_UINT:
                  ahbFormatProps->format = VK_FORMAT_D24_UNORM_S8_UINT;
                  break;
            case AHARDWAREBUFFER_FORMAT_D32_FLOAT:
                  ahbFormatProps->format = VK_FORMAT_D32_SFLOAT;
                  break;
            case AHARDWAREBUFFER_FORMAT_D32_FLOAT_S8_UINT:
                  ahbFormatProps->format = VK_FORMAT_D32_SFLOAT_S8_UINT;
                  break;
            case AHARDWAREBUFFER_FORMAT_S8_UINT:
                  ahbFormatProps->format = VK_FORMAT_S8_UINT;
                  break;
            default:
                  ahbFormatProps->format = VK_FORMAT_UNDEFINED;
        }
        ahbFormatProps->externalFormat = desc.format;

        // The formatFeatures member must include
        // VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT and at least one of
        // VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT or
        // VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT, and should include
        // VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT and
        // VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT.

        // org.skia.skqp.SkQPRunner#UnitTest_VulkanHardwareBuffer* requires the following:
        // VK_FORMAT_FEATURE_TRANSFER_SRC_BIT
        // VK_FORMAT_FEATURE_TRANSFER_DST_BIT
        // VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
        ahbFormatProps->formatFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;

        // "Implementations may not always be able to determine the color model,
        // numerical range, or chroma offsets of the image contents, so the values in
        // VkAndroidHardwareBufferFormatPropertiesANDROID are only suggestions.
        // Applications should treat these values as sensible defaults to use in the
        // absence of more reliable information obtained through some other means."

        ahbFormatProps->samplerYcbcrConversionComponents.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        ahbFormatProps->samplerYcbcrConversionComponents.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        ahbFormatProps->samplerYcbcrConversionComponents.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        ahbFormatProps->samplerYcbcrConversionComponents.a = VK_COMPONENT_SWIZZLE_IDENTITY;

#if !defined(HOST_BUILD)
#if defined(__ANDROID__) || defined(__linux__)
        if (android_format_is_yuv(desc.format)) {
            uint32_t drmFormat = grallocHelper->getFormatDrmFourcc(handle);
            if (drmFormat) {
                // The host renderer is not aware of the plane ordering for YUV formats used
                // in the guest and simply knows that the format "layout" is one of:
                //
                //  * VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
                //  * VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
                //  * VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM
                //
                // With this, the guest needs to adjust the component swizzle based on plane
                // ordering to ensure that the channels are interpreted correctly.
                //
                // From the Vulkan spec's "Sampler Y'CBCR Conversion" section:
                //
                //  * Y comes from the G-channel (after swizzle)
                //  * U (CB) comes from the B-channel (after swizzle)
                //  * V (CR) comes from the R-channel (after swizzle)
                //
                // See https://www.khronos.org/registry/vulkan/specs/1.3-extensions/html/vkspec.html#textures-sampler-YCbCr-conversion
                //
                // To match the above, the guest needs to swizzle such that:
                //
                //  * Y ends up in the G-channel
                //  * U (CB) ends up in the B-channel
                //  * V (CB) ends up in the R-channel
                switch (drmFormat) {
                    case DRM_FORMAT_NV12:
                        // NV12 is a Y-plane followed by a interleaved UV-plane and is
                        // VK_FORMAT_G8_B8R8_2PLANE_420_UNORM on the host.
                    case DRM_FORMAT_P010:
                        // P010 is a Y-plane followed by a interleaved UV-plane and is
                        // VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 on the host.
                        break;

                    case DRM_FORMAT_NV21:
                        // NV21 is a Y-plane followed by a interleaved VU-plane and is
                        // VK_FORMAT_G8_B8R8_2PLANE_420_UNORM on the host.
                    case DRM_FORMAT_YVU420:
                        // YV12 is a Y-plane, then a V-plane, and then a U-plane and is
                        // VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM on the host.
                    case DRM_FORMAT_YVU420_ANDROID:
                        // DRM_FORMAT_YVU420_ANDROID is the same as DRM_FORMAT_YVU420 with
                        // Android's extra alignement requirements.
                        ahbFormatProps->samplerYcbcrConversionComponents.r = VK_COMPONENT_SWIZZLE_B;
                        ahbFormatProps->samplerYcbcrConversionComponents.b = VK_COMPONENT_SWIZZLE_R;
                        break;

                    default:
                        ALOGE("%s: Unhandled YUV drm format:%" PRIu32, __FUNCTION__, drmFormat);
                        break;
                }
            }
        }
#endif
#endif

        ahbFormatProps->suggestedYcbcrModel =
            android_format_is_yuv(desc.format) ?
                VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601 :
                VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
        ahbFormatProps->suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;

        ahbFormatProps->suggestedXChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        ahbFormatProps->suggestedYChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    }

    uint32_t colorBufferHandle =
        grallocHelper->getHostHandle(handle);
    if (!colorBufferHandle) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    pProperties->allocationSize =
        grallocHelper->getAllocatedSize(handle);

    return VK_SUCCESS;
}

// Based on Intel ANV implementation.
VkResult getMemoryAndroidHardwareBufferANDROID(struct AHardwareBuffer **pBuffer) {

   /* Some quotes from Vulkan spec:
    *
    * "If the device memory was created by importing an Android hardware
    * buffer, vkGetMemoryAndroidHardwareBufferANDROID must return that same
    * Android hardware buffer object."
    *
    * "VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID must
    * have been included in VkExportMemoryAllocateInfo::handleTypes when
    * memory was created."
    */

    if (!pBuffer) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!(*pBuffer)) return VK_ERROR_OUT_OF_HOST_MEMORY;

    AHardwareBuffer_acquire(*pBuffer);
    return VK_SUCCESS;
}

VkResult importAndroidHardwareBuffer(
    Gralloc* grallocHelper,
    const VkImportAndroidHardwareBufferInfoANDROID* info,
    struct AHardwareBuffer **importOut) {

    if (!info || !info->buffer) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    uint32_t colorBufferHandle =
        grallocHelper->getHostHandle(
            AHardwareBuffer_getNativeHandle(info->buffer));
    if (!colorBufferHandle) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    auto ahb = info->buffer;

    AHardwareBuffer_acquire(ahb);

    if (importOut) *importOut = ahb;

    return VK_SUCCESS;
}

VkResult createAndroidHardwareBuffer(
    bool hasDedicatedImage,
    bool hasDedicatedBuffer,
    const VkExtent3D& imageExtent,
    uint32_t imageLayers,
    VkFormat imageFormat,
    VkImageUsageFlags imageUsage,
    VkImageCreateFlags imageCreateFlags,
    VkDeviceSize bufferSize,
    VkDeviceSize allocationInfoAllocSize,
    struct AHardwareBuffer **out) {

    uint32_t w = 0;
    uint32_t h = 1;
    uint32_t layers = 1;
    uint32_t format = 0;
    uint64_t usage = 0;

    /* If caller passed dedicated information. */
    if (hasDedicatedImage) {
       w = imageExtent.width;
       h = imageExtent.height;
       layers = imageLayers;
       format = android_format_from_vk(imageFormat);
       usage = getAndroidHardwareBufferUsageFromVkUsage(imageCreateFlags, imageUsage);
    } else if (hasDedicatedBuffer) {
       w = bufferSize;
       format = AHARDWAREBUFFER_FORMAT_BLOB;
       usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
               AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
               AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    } else {
       w = allocationInfoAllocSize;
       format = AHARDWAREBUFFER_FORMAT_BLOB;
       usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
               AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
               AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    }

    struct AHardwareBuffer *ahw = NULL;
    struct AHardwareBuffer_Desc desc = {
        .width = w,
        .height = h,
        .layers = layers,
        .format = format,
        .usage = usage,
    };

    if (AHardwareBuffer_allocate(&desc, &ahw) != 0) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    *out = ahw;

    return VK_SUCCESS;
}

}  // namespace vk
}  // namespace gfxstream
