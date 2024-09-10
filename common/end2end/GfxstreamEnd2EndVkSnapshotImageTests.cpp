// Copyright (C) 2023 The Android Open Source Project
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

#include <string>

#include "GfxstreamEnd2EndTestUtils.h"
#include "GfxstreamEnd2EndTests.h"

namespace gfxstream {
namespace tests {
namespace {

using testing::Eq;
using testing::Ge;
using testing::IsEmpty;
using testing::IsNull;
using testing::Not;
using testing::NotNull;

class GfxstreamEnd2EndVkSnapshotImageTest : public GfxstreamEnd2EndTest {};

TEST_P(GfxstreamEnd2EndVkSnapshotImageTest, PreserveImageHandle) {
    auto [instance, physicalDevice, device, queue, queueFamilyIndex] =
        GFXSTREAM_ASSERT(SetUpTypicalVkTestEnvironment());

    const uint32_t width = 32;
    const uint32_t height = 32;

    const vkhpp::ImageCreateInfo imageCreateInfo = {
        .pNext = nullptr,
        .imageType = vkhpp::ImageType::e2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = vkhpp::Format::eR8G8B8A8Unorm,
        .tiling = vkhpp::ImageTiling::eOptimal,
        .initialLayout = vkhpp::ImageLayout::eUndefined,
        .usage = vkhpp::ImageUsageFlagBits::eSampled | vkhpp::ImageUsageFlagBits::eTransferDst |
                 vkhpp::ImageUsageFlagBits::eTransferSrc,
        .sharingMode = vkhpp::SharingMode::eExclusive,
        .samples = vkhpp::SampleCountFlagBits::e1,
    };
    auto image = device->createImageUnique(imageCreateInfo).value;

    vkhpp::MemoryRequirements imageMemoryRequirements{};
    device->getImageMemoryRequirements(*image, &imageMemoryRequirements);

    const uint32_t imageMemoryIndex = utils::getMemoryType(
        physicalDevice, imageMemoryRequirements, vkhpp::MemoryPropertyFlagBits::eDeviceLocal);
    ASSERT_THAT(imageMemoryIndex, Not(Eq(-1)));

    const vkhpp::MemoryAllocateInfo imageMemoryAllocateInfo = {
        .allocationSize = imageMemoryRequirements.size,
        .memoryTypeIndex = imageMemoryIndex,
    };

    auto imageMemory = device->allocateMemoryUnique(imageMemoryAllocateInfo).value;
    ASSERT_THAT(imageMemory, IsValidHandle());

    SnapshotSaveAndLoad();

    ASSERT_THAT(device->bindImageMemory(*image, *imageMemory, 0), IsVkSuccess());
}

// b/346415931
// We used to have an issue that the handles mismatch when running more device
// create calls. The first device always work but the second might break.
TEST_P(GfxstreamEnd2EndVkSnapshotImageTest, MultipleDevicesPreserveHandles) {
    auto [instance, physicalDevice, device, queue, queueFamilyIndex] =
        GFXSTREAM_ASSERT(SetUpTypicalVkTestEnvironment());

    uint32_t graphicsQueueFamilyIndex = -1;
    {
        const auto props = physicalDevice.getQueueFamilyProperties();
        for (uint32_t i = 0; i < props.size(); i++) {
            const auto& prop = props[i];
            if (prop.queueFlags & vkhpp::QueueFlagBits::eGraphics) {
                graphicsQueueFamilyIndex = i;
                break;
            }
        }
    }
    ASSERT_THAT(graphicsQueueFamilyIndex, Not(Eq(-1)));
    const float queuePriority = 1.0f;
    const vkhpp::DeviceQueueCreateInfo deviceQueueCreateInfo = {
        .queueFamilyIndex = graphicsQueueFamilyIndex,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };
    std::vector<const char*> deviceExtensions = {
        VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
    };
    const vkhpp::DeviceCreateInfo deviceCreateInfo = {
        .pNext = nullptr,
        .pQueueCreateInfos = &deviceQueueCreateInfo,
        .queueCreateInfoCount = 1,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size()),
        .ppEnabledExtensionNames = deviceExtensions.data(),
    };
    auto device2 = physicalDevice.createDeviceUnique(deviceCreateInfo).value;
    ASSERT_THAT(device2, IsValidHandle());

    const uint32_t width = 32;
    const uint32_t height = 32;

    const vkhpp::ImageCreateInfo imageCreateInfo = {
        .pNext = nullptr,
        .imageType = vkhpp::ImageType::e2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = vkhpp::Format::eR8G8B8A8Unorm,
        .tiling = vkhpp::ImageTiling::eOptimal,
        .initialLayout = vkhpp::ImageLayout::eUndefined,
        .usage = vkhpp::ImageUsageFlagBits::eSampled | vkhpp::ImageUsageFlagBits::eTransferDst |
                 vkhpp::ImageUsageFlagBits::eTransferSrc,
        .sharingMode = vkhpp::SharingMode::eExclusive,
        .samples = vkhpp::SampleCountFlagBits::e1,
    };
    auto image = device->createImageUnique(imageCreateInfo).value;

    vkhpp::MemoryRequirements imageMemoryRequirements{};
    device->getImageMemoryRequirements(*image, &imageMemoryRequirements);

    const uint32_t imageMemoryIndex = utils::getMemoryType(
        physicalDevice, imageMemoryRequirements, vkhpp::MemoryPropertyFlagBits::eDeviceLocal);
    ASSERT_THAT(imageMemoryIndex, Not(Eq(-1)));

    const vkhpp::MemoryAllocateInfo imageMemoryAllocateInfo = {
        .allocationSize = imageMemoryRequirements.size,
        .memoryTypeIndex = imageMemoryIndex,
    };

    auto imageMemory = device->allocateMemoryUnique(imageMemoryAllocateInfo).value;
    ASSERT_THAT(imageMemory, IsValidHandle());

    ASSERT_THAT(device->bindImageMemory(*image, *imageMemory, 0), IsVkSuccess());

    // No device lost on snapshot load.
    SnapshotSaveAndLoad();
}

TEST_P(GfxstreamEnd2EndVkSnapshotImageTest, ImageViewDependency) {
    auto [instance, physicalDevice, device, queue, queueFamilyIndex] =
        GFXSTREAM_ASSERT(SetUpTypicalVkTestEnvironment());

    const uint32_t width = 32;
    const uint32_t height = 32;

    const vkhpp::ImageCreateInfo imageCreateInfo = {
        .pNext = nullptr,
        .imageType = vkhpp::ImageType::e2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = vkhpp::Format::eR8G8B8A8Unorm,
        .tiling = vkhpp::ImageTiling::eOptimal,
        .initialLayout = vkhpp::ImageLayout::eUndefined,
        .usage = vkhpp::ImageUsageFlagBits::eSampled | vkhpp::ImageUsageFlagBits::eTransferDst |
                 vkhpp::ImageUsageFlagBits::eTransferSrc,
        .sharingMode = vkhpp::SharingMode::eExclusive,
        .samples = vkhpp::SampleCountFlagBits::e1,
    };
    auto image = device->createImageUnique(imageCreateInfo).value;
    ASSERT_THAT(image, IsValidHandle());

    vkhpp::MemoryRequirements imageMemoryRequirements{};
    device->getImageMemoryRequirements(*image, &imageMemoryRequirements);

    const uint32_t imageMemoryIndex = utils::getMemoryType(
        physicalDevice, imageMemoryRequirements, vkhpp::MemoryPropertyFlagBits::eDeviceLocal);
    ASSERT_THAT(imageMemoryIndex, Not(Eq(-1)));

    const vkhpp::MemoryAllocateInfo imageMemoryAllocateInfo = {
        .allocationSize = imageMemoryRequirements.size,
        .memoryTypeIndex = imageMemoryIndex,
    };

    auto imageMemory = device->allocateMemoryUnique(imageMemoryAllocateInfo).value;
    ASSERT_THAT(imageMemory, IsValidHandle());

    ASSERT_THAT(device->bindImageMemory(*image, *imageMemory, 0), IsVkSuccess());

    // b/331677615
    // Create and delete a buffer handle right before creating image view.
    // Gfxstream recycle handles. We trick the VkImageView handle to collide with
    // a destroyed buffer handle and verify there is no bug snapshotting recycled
    // handles.
    const vkhpp::BufferCreateInfo bufferCreateInfo = {
        .size = 1024,
        .usage = vkhpp::BufferUsageFlagBits::eTransferSrc,
    };
    auto buffer = device->createBufferUnique(bufferCreateInfo).value;
    ASSERT_THAT(buffer, IsValidHandle());
    buffer.reset();

    const vkhpp::ImageViewCreateInfo imageViewCreateInfo = {
        .image = *image,
        .viewType = vkhpp::ImageViewType::e2D,
        .format = vkhpp::Format::eR8G8B8A8Unorm,
        .subresourceRange =
            {
                .aspectMask = vkhpp::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    auto imageView = device->createImageViewUnique(imageViewCreateInfo).value;
    ASSERT_THAT(imageView, IsValidHandle());
    // Make sure it doesn't crash on load
    SnapshotSaveAndLoad();
}

// Use vkBindImageMemory2 instead of vkBindImageMemory
TEST_P(GfxstreamEnd2EndVkSnapshotImageTest, ImageViewDependency2) {
    auto [instance, physicalDevice, device, queue, queueFamilyIndex] =
        GFXSTREAM_ASSERT(SetUpTypicalVkTestEnvironment());

    const uint32_t width = 32;
    const uint32_t height = 32;

    const vkhpp::ImageCreateInfo imageCreateInfo = {
        .pNext = nullptr,
        .imageType = vkhpp::ImageType::e2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = vkhpp::Format::eR8G8B8A8Unorm,
        .tiling = vkhpp::ImageTiling::eOptimal,
        .initialLayout = vkhpp::ImageLayout::eUndefined,
        .usage = vkhpp::ImageUsageFlagBits::eSampled | vkhpp::ImageUsageFlagBits::eTransferDst |
                 vkhpp::ImageUsageFlagBits::eTransferSrc,
        .sharingMode = vkhpp::SharingMode::eExclusive,
        .samples = vkhpp::SampleCountFlagBits::e1,
    };
    auto image = device->createImageUnique(imageCreateInfo).value;
    ASSERT_THAT(image, IsValidHandle());

    vkhpp::MemoryRequirements imageMemoryRequirements{};
    device->getImageMemoryRequirements(*image, &imageMemoryRequirements);

    const uint32_t imageMemoryIndex = utils::getMemoryType(
        physicalDevice, imageMemoryRequirements, vkhpp::MemoryPropertyFlagBits::eDeviceLocal);
    ASSERT_THAT(imageMemoryIndex, Not(Eq(-1)));

    const vkhpp::MemoryAllocateInfo imageMemoryAllocateInfo = {
        .allocationSize = imageMemoryRequirements.size,
        .memoryTypeIndex = imageMemoryIndex,
    };

    auto imageMemory = device->allocateMemoryUnique(imageMemoryAllocateInfo).value;
    ASSERT_THAT(imageMemory, IsValidHandle());

    const vkhpp::BindImageMemoryInfo imageBindMemoryInfo = {
        .pNext = nullptr,
        .image = *image,
        .memory = *imageMemory,
        .memoryOffset = 0,
    };

    ASSERT_THAT(device->bindImageMemory2({imageBindMemoryInfo}), IsVkSuccess());

    // b/331677615
    // Create and delete a buffer handle right before creating image view.
    // Gfxstream recycle handles. We trick the VkImageView handle to collide with
    // a destroyed buffer handle and verify there is no bug snapshotting recycled
    // handles.
    const vkhpp::BufferCreateInfo bufferCreateInfo = {
        .size = 1024,
        .usage = vkhpp::BufferUsageFlagBits::eTransferSrc,
    };
    auto buffer = device->createBufferUnique(bufferCreateInfo).value;
    ASSERT_THAT(buffer, IsValidHandle());
    buffer.reset();

    const vkhpp::ImageViewCreateInfo imageViewCreateInfo = {
        .image = *image,
        .viewType = vkhpp::ImageViewType::e2D,
        .format = vkhpp::Format::eR8G8B8A8Unorm,
        .subresourceRange =
            {
                .aspectMask = vkhpp::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    auto imageView = device->createImageViewUnique(imageViewCreateInfo).value;
    ASSERT_THAT(imageView, IsValidHandle());
    // Make sure it doesn't crash on load
    SnapshotSaveAndLoad();
}

TEST_P(GfxstreamEnd2EndVkSnapshotImageTest, MultiSampleImage) {
    auto [instance, physicalDevice, device, queue, queueFamilyIndex] =
        GFXSTREAM_ASSERT(SetUpTypicalVkTestEnvironment());

    const uint32_t width = 32;
    const uint32_t height = 32;

    const vkhpp::ImageCreateInfo imageCreateInfo = {
        .pNext = nullptr,
        .imageType = vkhpp::ImageType::e2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = vkhpp::Format::eR8G8B8A8Unorm,
        .tiling = vkhpp::ImageTiling::eOptimal,
        .initialLayout = vkhpp::ImageLayout::eUndefined,
        .usage = vkhpp::ImageUsageFlagBits::eColorAttachment |
                 vkhpp::ImageUsageFlagBits::eTransferDst | vkhpp::ImageUsageFlagBits::eTransferSrc,
        .sharingMode = vkhpp::SharingMode::eExclusive,
        .samples = vkhpp::SampleCountFlagBits::e8,
    };
    auto image = device->createImageUnique(imageCreateInfo).value;
    ASSERT_THAT(image, IsValidHandle());

    vkhpp::MemoryRequirements imageMemoryRequirements{};
    device->getImageMemoryRequirements(*image, &imageMemoryRequirements);

    const uint32_t imageMemoryIndex = utils::getMemoryType(
        physicalDevice, imageMemoryRequirements, vkhpp::MemoryPropertyFlagBits::eDeviceLocal);
    ASSERT_THAT(imageMemoryIndex, Not(Eq(-1)));

    const vkhpp::MemoryAllocateInfo imageMemoryAllocateInfo = {
        .allocationSize = imageMemoryRequirements.size,
        .memoryTypeIndex = imageMemoryIndex,
    };

    auto imageMemory = device->allocateMemoryUnique(imageMemoryAllocateInfo).value;
    ASSERT_THAT(imageMemory, IsValidHandle());

    // Make sure it doesn't crash on load
    SnapshotSaveAndLoad();
}

TEST_P(GfxstreamEnd2EndVkSnapshotImageTest, ImageViewDependencyWithDedicatedMemory) {
    auto [instance, physicalDevice, device, queue, queueFamilyIndex] =
        GFXSTREAM_ASSERT(SetUpTypicalVkTestEnvironment());

    const uint32_t width = 32;
    const uint32_t height = 32;

    const vkhpp::ImageCreateInfo imageCreateInfo = {
        .pNext = nullptr,
        .imageType = vkhpp::ImageType::e2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = vkhpp::Format::eR8G8B8A8Unorm,
        .tiling = vkhpp::ImageTiling::eOptimal,
        .initialLayout = vkhpp::ImageLayout::eUndefined,
        .usage = vkhpp::ImageUsageFlagBits::eSampled | vkhpp::ImageUsageFlagBits::eTransferDst |
                 vkhpp::ImageUsageFlagBits::eTransferSrc,
        .sharingMode = vkhpp::SharingMode::eExclusive,
        .samples = vkhpp::SampleCountFlagBits::e1,
    };
    auto image = device->createImageUnique(imageCreateInfo).value;
    ASSERT_THAT(image, IsValidHandle());

    vkhpp::MemoryRequirements imageMemoryRequirements{};
    device->getImageMemoryRequirements(*image, &imageMemoryRequirements);

    const uint32_t imageMemoryIndex = utils::getMemoryType(
        physicalDevice, imageMemoryRequirements, vkhpp::MemoryPropertyFlagBits::eDeviceLocal);
    ASSERT_THAT(imageMemoryIndex, Not(Eq(-1)));

    const vkhpp::MemoryDedicatedAllocateInfo dedicatedAllocateInfo = {
        .image = *image,
    };

    const vkhpp::MemoryAllocateInfo imageMemoryAllocateInfo = {
        .pNext = &dedicatedAllocateInfo,
        .allocationSize = imageMemoryRequirements.size,
        .memoryTypeIndex = imageMemoryIndex,
    };

    auto imageMemory = device->allocateMemoryUnique(imageMemoryAllocateInfo).value;
    ASSERT_THAT(imageMemory, IsValidHandle());

    ASSERT_THAT(device->bindImageMemory(*image, *imageMemory, 0), IsVkSuccess());

    const vkhpp::ImageViewCreateInfo imageViewCreateInfo = {
        .image = *image,
        .viewType = vkhpp::ImageViewType::e2D,
        .format = vkhpp::Format::eR8G8B8A8Unorm,
        .subresourceRange =
            {
                .aspectMask = vkhpp::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    auto imageView = device->createImageViewUnique(imageViewCreateInfo).value;
    ASSERT_THAT(imageView, IsValidHandle());
    // Make sure it doesn't crash on load
    SnapshotSaveAndLoad();
}

TEST_P(GfxstreamEnd2EndVkSnapshotImageTest, ImageContent) {
    static constexpr int kWidth = 256;
    static constexpr int kHeight = 256;
    static constexpr vkhpp::DeviceSize kSize = 4 * kWidth * kHeight;

    std::vector<uint8_t> srcBufferContent(kSize);
    for (size_t i = 0; i < kSize; i++) {
        srcBufferContent[i] = static_cast<uint8_t>(i & 0xff);
    }
    TypicalVkTestEnvironment testEnvironment = GFXSTREAM_ASSERT(SetUpTypicalVkTestEnvironment());
    auto& instance = testEnvironment.instance;
    auto& physicalDevice = testEnvironment.physicalDevice;
    auto& device = testEnvironment.device;
    auto& queue = testEnvironment.queue;
    auto queueFamilyIndex = testEnvironment.queueFamilyIndex;

    // Staging buffer
    const vkhpp::BufferCreateInfo bufferCreateInfo = {
        .size = static_cast<VkDeviceSize>(kSize),
        .usage = vkhpp::BufferUsageFlagBits::eTransferSrc,
        .sharingMode = vkhpp::SharingMode::eExclusive,
    };
    auto stagingBuffer = device->createBufferUnique(bufferCreateInfo).value;
    ASSERT_THAT(stagingBuffer, IsValidHandle());

    vkhpp::MemoryRequirements stagingBufferMemoryRequirements{};
    device->getBufferMemoryRequirements(*stagingBuffer, &stagingBufferMemoryRequirements);

    const auto stagingBufferMemoryType = utils::getMemoryType(
        physicalDevice, stagingBufferMemoryRequirements,
        vkhpp::MemoryPropertyFlagBits::eHostVisible | vkhpp::MemoryPropertyFlagBits::eHostCoherent);

    // Staging memory
    const vkhpp::MemoryAllocateInfo stagingBufferMemoryAllocateInfo = {
        .allocationSize = stagingBufferMemoryRequirements.size,
        .memoryTypeIndex = stagingBufferMemoryType,
    };
    auto stagingBufferMemory = device->allocateMemoryUnique(stagingBufferMemoryAllocateInfo).value;
    ASSERT_THAT(stagingBufferMemory, IsValidHandle());
    ASSERT_THAT(device->bindBufferMemory(*stagingBuffer, *stagingBufferMemory, 0), IsVkSuccess());

    // Fill memory content
    void* mapped = nullptr;
    auto mapResult =
        device->mapMemory(*stagingBufferMemory, 0, VK_WHOLE_SIZE, vkhpp::MemoryMapFlags{}, &mapped);
    ASSERT_THAT(mapResult, IsVkSuccess());
    ASSERT_THAT(mapped, NotNull());

    auto* bytes = reinterpret_cast<uint8_t*>(mapped);
    std::memcpy(bytes, srcBufferContent.data(), kSize);

    const vkhpp::MappedMemoryRange range = {
        .memory = *stagingBufferMemory,
        .offset = 0,
        .size = kSize,
    };
    device->unmapMemory(*stagingBufferMemory);

    // Image
    const vkhpp::ImageCreateInfo imageCreateInfo = {
        .pNext = nullptr,
        .imageType = vkhpp::ImageType::e2D,
        .extent.width = kWidth,
        .extent.height = kHeight,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = vkhpp::Format::eR8G8B8A8Unorm,
        .tiling = vkhpp::ImageTiling::eOptimal,
        .initialLayout = vkhpp::ImageLayout::ePreinitialized,
        .usage = vkhpp::ImageUsageFlagBits::eTransferDst | vkhpp::ImageUsageFlagBits::eTransferSrc,
        .sharingMode = vkhpp::SharingMode::eExclusive,
        .samples = vkhpp::SampleCountFlagBits::e1,
    };
    auto image = device->createImageUnique(imageCreateInfo).value;
    ASSERT_THAT(image, IsValidHandle());

    vkhpp::MemoryRequirements imageMemoryRequirements{};
    device->getImageMemoryRequirements(*image, &imageMemoryRequirements);

    const uint32_t imageMemoryIndex = utils::getMemoryType(
        physicalDevice, imageMemoryRequirements, vkhpp::MemoryPropertyFlagBits::eDeviceLocal);
    ASSERT_THAT(imageMemoryIndex, Not(Eq(-1)));

    const vkhpp::MemoryAllocateInfo imageMemoryAllocateInfo = {
        .allocationSize = imageMemoryRequirements.size,
        .memoryTypeIndex = imageMemoryIndex,
    };

    auto imageMemory = device->allocateMemoryUnique(imageMemoryAllocateInfo).value;
    ASSERT_THAT(imageMemory, IsValidHandle());

    ASSERT_THAT(device->bindImageMemory(*image, *imageMemory, 0), IsVkSuccess());

    // Command buffer
    const vkhpp::CommandPoolCreateInfo commandPoolCreateInfo = {
        .queueFamilyIndex = queueFamilyIndex,
    };

    auto commandPool = device->createCommandPoolUnique(commandPoolCreateInfo).value;
    ASSERT_THAT(commandPool, IsValidHandle());

    const vkhpp::CommandBufferAllocateInfo commandBufferAllocateInfo = {
        .level = vkhpp::CommandBufferLevel::ePrimary,
        .commandPool = *commandPool,
        .commandBufferCount = 1,
    };
    auto commandBuffers = device->allocateCommandBuffersUnique(commandBufferAllocateInfo).value;
    ASSERT_THAT(commandBuffers, Not(IsEmpty()));
    auto commandBuffer = std::move(commandBuffers[0]);
    ASSERT_THAT(commandBuffer, IsValidHandle());

    const vkhpp::CommandBufferBeginInfo commandBufferBeginInfo = {
        .flags = vkhpp::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    commandBuffer->begin(commandBufferBeginInfo);

    const vkhpp::ImageMemoryBarrier barrier{
        .oldLayout = vkhpp::ImageLayout::eUndefined,
        .newLayout = vkhpp::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = *image,
        .subresourceRange =
            {
                .aspectMask = vkhpp::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1,
            },
    };

    commandBuffer->pipelineBarrier(vkhpp::PipelineStageFlagBits::eAllCommands,
                                   vkhpp::PipelineStageFlagBits::eAllCommands,
                                   vkhpp::DependencyFlags(), nullptr, nullptr, barrier);

    const vkhpp::BufferImageCopy bufferImageCopy = {
        .imageSubresource =
            {
                .aspectMask = vkhpp::ImageAspectFlagBits::eColor,
                .layerCount = 1,
            },
        .imageExtent =
            {
                .width = kWidth,
                .height = kHeight,
                .depth = 1,
            },
    };
    commandBuffer->copyBufferToImage(*stagingBuffer, *image,
                                     vkhpp::ImageLayout::eTransferDstOptimal, 1, &bufferImageCopy);

    commandBuffer->end();

    auto transferFence = device->createFenceUnique(vkhpp::FenceCreateInfo()).value;
    ASSERT_THAT(transferFence, IsValidHandle());

    // Execute the command to copy image
    const vkhpp::SubmitInfo submitInfo = {
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer.get(),
    };
    queue.submit(submitInfo, *transferFence);

    auto waitResult = device->waitForFences(*transferFence, VK_TRUE, 3000000000L);
    ASSERT_THAT(waitResult, IsVkSuccess());

    // Snapshot
    SnapshotSaveAndLoad();

    std::vector<uint8_t> dst(kSize);
    utils::readImageData(*image, kWidth, kHeight, vkhpp::ImageLayout::eTransferDstOptimal,
                         dst.data(), kSize, testEnvironment);

    for (uint32_t i = 0; i < kSize; ++i) {
        ASSERT_THAT(dst[i], Eq(srcBufferContent[i]));
    }
}

INSTANTIATE_TEST_CASE_P(GfxstreamEnd2EndTests, GfxstreamEnd2EndVkSnapshotImageTest,
                        ::testing::ValuesIn({
                            TestParams{
                                .with_gl = false,
                                .with_vk = true,
                                .with_features = {"VulkanSnapshots"},
                            },
                        }),
                        &GetTestName);

}  // namespace
}  // namespace tests
}  // namespace gfxstream
