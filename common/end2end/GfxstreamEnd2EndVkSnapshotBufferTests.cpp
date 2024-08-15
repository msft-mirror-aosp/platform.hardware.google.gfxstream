// Copyright (C) 2024 The Android Open Source Project
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

class GfxstreamEnd2EndVkSnapshotBufferTest : public GfxstreamEnd2EndTest {};

TEST_P(GfxstreamEnd2EndVkSnapshotBufferTest, DeviceLocalBufferContent) {
    static constexpr vkhpp::DeviceSize kSize = 256;

    std::vector<uint8_t> srcBufferContent(kSize);
    for (size_t i = 0; i < kSize; i++) {
        srcBufferContent[i] = static_cast<uint8_t>(i & 0xff);
    }
    auto [instance, physicalDevice, device, queue, queueFamilyIndex] =
        GFXSTREAM_ASSERT(SetUpTypicalVkTestEnvironment());

    // Staging buffer
    const vkhpp::BufferCreateInfo stagingBufferCreateInfo = {
        .size = static_cast<VkDeviceSize>(kSize),
        .usage = vkhpp::BufferUsageFlagBits::eTransferSrc,
        .sharingMode = vkhpp::SharingMode::eExclusive,
    };
    auto stagingBuffer = device->createBufferUnique(stagingBufferCreateInfo).value;
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

    // Vertex buffer
    const vkhpp::BufferCreateInfo vertexBufferCreateInfo = {
        .size = static_cast<VkDeviceSize>(kSize),
        .usage = vkhpp::BufferUsageFlagBits::eVertexBuffer |
                 vkhpp::BufferUsageFlagBits::eTransferSrc |
                 vkhpp::BufferUsageFlagBits::eTransferDst,
        .sharingMode = vkhpp::SharingMode::eExclusive,
    };
    auto vertexBuffer = device->createBufferUnique(vertexBufferCreateInfo).value;
    ASSERT_THAT(vertexBuffer, IsValidHandle());

    vkhpp::MemoryRequirements vertexBufferMemoryRequirements{};
    device->getBufferMemoryRequirements(*vertexBuffer, &vertexBufferMemoryRequirements);

    const auto vertexBufferMemoryType =
        utils::getMemoryType(physicalDevice, vertexBufferMemoryRequirements,
                             vkhpp::MemoryPropertyFlagBits::eDeviceLocal);

    // Vertex memory
    const vkhpp::MemoryAllocateInfo vertexBufferMemoryAllocateInfo = {
        .allocationSize = vertexBufferMemoryRequirements.size,
        .memoryTypeIndex = vertexBufferMemoryType,
    };
    auto vertexBufferMemory = device->allocateMemoryUnique(vertexBufferMemoryAllocateInfo).value;
    ASSERT_THAT(vertexBufferMemory, IsValidHandle());
    ASSERT_THAT(device->bindBufferMemory(*vertexBuffer, *vertexBufferMemory, 0), IsVkSuccess());

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
    const vkhpp::BufferCopy bufferCopy = {
        .size = kSize,
    };
    commandBuffer->copyBuffer(*stagingBuffer, *vertexBuffer, 1, &bufferCopy);
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

    // Read-back buffer
    const vkhpp::BufferCreateInfo readbackBufferCreateInfo = {
        .size = static_cast<VkDeviceSize>(kSize),
        .usage = vkhpp::BufferUsageFlagBits::eTransferDst,
        .sharingMode = vkhpp::SharingMode::eExclusive,
    };
    auto readbackBuffer = device->createBufferUnique(readbackBufferCreateInfo).value;
    ASSERT_THAT(readbackBuffer, IsValidHandle());

    vkhpp::MemoryRequirements readbackBufferMemoryRequirements{};
    device->getBufferMemoryRequirements(*readbackBuffer, &readbackBufferMemoryRequirements);

    const auto readbackBufferMemoryType = utils::getMemoryType(
        physicalDevice, readbackBufferMemoryRequirements,
        vkhpp::MemoryPropertyFlagBits::eHostVisible | vkhpp::MemoryPropertyFlagBits::eHostCoherent);

    // Read-back memory
    const vkhpp::MemoryAllocateInfo readbackBufferMemoryAllocateInfo = {
        .allocationSize = readbackBufferMemoryRequirements.size,
        .memoryTypeIndex = readbackBufferMemoryType,
    };
    auto readbackBufferMemory =
        device->allocateMemoryUnique(readbackBufferMemoryAllocateInfo).value;
    ASSERT_THAT(readbackBufferMemory, IsValidHandle());
    ASSERT_THAT(device->bindBufferMemory(*readbackBuffer, *readbackBufferMemory, 0), IsVkSuccess());

    auto readbackCommandBuffers =
        device->allocateCommandBuffersUnique(commandBufferAllocateInfo).value;
    ASSERT_THAT(readbackCommandBuffers, Not(IsEmpty()));
    auto readbackCommandBuffer = std::move(readbackCommandBuffers[0]);
    ASSERT_THAT(readbackCommandBuffer, IsValidHandle());

    readbackCommandBuffer->begin(commandBufferBeginInfo);
    readbackCommandBuffer->copyBuffer(*vertexBuffer, *readbackBuffer, 1, &bufferCopy);
    readbackCommandBuffer->end();

    auto readbackFence = device->createFenceUnique(vkhpp::FenceCreateInfo()).value;
    ASSERT_THAT(readbackCommandBuffer, IsValidHandle());

    // Execute the command to copy image back to buffer
    const vkhpp::SubmitInfo readbackSubmitInfo = {
        .commandBufferCount = 1,
        .pCommandBuffers = &readbackCommandBuffer.get(),
    };
    queue.submit(readbackSubmitInfo, *readbackFence);

    auto readbackWaitResult = device->waitForFences(*readbackFence, VK_TRUE, 3000000000L);
    ASSERT_THAT(readbackWaitResult, IsVkSuccess());

    // Verify content
    mapResult = device->mapMemory(*readbackBufferMemory, 0, VK_WHOLE_SIZE, vkhpp::MemoryMapFlags{},
                                  &mapped);
    ASSERT_THAT(mapResult, IsVkSuccess());
    ASSERT_THAT(mapped, NotNull());
    bytes = reinterpret_cast<uint8_t*>(mapped);

    for (uint32_t i = 0; i < kSize; ++i) {
        ASSERT_THAT(bytes[i], Eq(srcBufferContent[i]));
    }
    device->unmapMemory(*readbackBufferMemory);
}

TEST_P(GfxstreamEnd2EndVkSnapshotBufferTest, HostVisibleBufferContent) {
    static constexpr vkhpp::DeviceSize kSize = 256;

    std::vector<uint8_t> srcBufferContent(kSize);
    for (size_t i = 0; i < kSize; i++) {
        srcBufferContent[i] = static_cast<uint8_t>(i & 0xff);
    }
    auto [instance, physicalDevice, device, queue, queueFamilyIndex] =
        GFXSTREAM_ASSERT(SetUpTypicalVkTestEnvironment());

    // Vertex buffer
    const vkhpp::BufferCreateInfo uniformBufferCreateInfo = {
        .size = static_cast<VkDeviceSize>(kSize),
        .usage = vkhpp::BufferUsageFlagBits::eUniformBuffer,
        .sharingMode = vkhpp::SharingMode::eExclusive,
    };
    auto uniformBuffer = device->createBufferUnique(uniformBufferCreateInfo).value;
    ASSERT_THAT(uniformBuffer, IsValidHandle());

    vkhpp::MemoryRequirements uniformBufferMemoryRequirements{};
    device->getBufferMemoryRequirements(*uniformBuffer, &uniformBufferMemoryRequirements);

    const auto uniformBufferMemoryType = utils::getMemoryType(
        physicalDevice, uniformBufferMemoryRequirements,
        vkhpp::MemoryPropertyFlagBits::eHostVisible | vkhpp::MemoryPropertyFlagBits::eHostCoherent);

    // Vertex memory
    const vkhpp::MemoryAllocateInfo uniformBufferMemoryAllocateInfo = {
        .allocationSize = uniformBufferMemoryRequirements.size,
        .memoryTypeIndex = uniformBufferMemoryType,
    };
    auto uniformBufferMemory = device->allocateMemoryUnique(uniformBufferMemoryAllocateInfo).value;
    ASSERT_THAT(uniformBufferMemory, IsValidHandle());
    ASSERT_THAT(device->bindBufferMemory(*uniformBuffer, *uniformBufferMemory, 0), IsVkSuccess());

    // Fill memory content
    void* mapped = nullptr;
    auto mapResult =
        device->mapMemory(*uniformBufferMemory, 0, VK_WHOLE_SIZE, vkhpp::MemoryMapFlags{}, &mapped);
    ASSERT_THAT(mapResult, IsVkSuccess());
    ASSERT_THAT(mapped, NotNull());

    auto* bytes = reinterpret_cast<uint8_t*>(mapped);
    std::memcpy(bytes, srcBufferContent.data(), kSize);

    device->unmapMemory(*uniformBufferMemory);

    // We need to unmap before snapshot due to limitations of the testing framework.
    SnapshotSaveAndLoad();

    // Verify content
    mapResult =
        device->mapMemory(*uniformBufferMemory, 0, VK_WHOLE_SIZE, vkhpp::MemoryMapFlags{}, &mapped);
    ASSERT_THAT(mapResult, IsVkSuccess());
    ASSERT_THAT(mapped, NotNull());
    bytes = reinterpret_cast<uint8_t*>(mapped);

    for (uint32_t i = 0; i < kSize; ++i) {
        ASSERT_THAT(bytes[i], Eq(srcBufferContent[i]));
    }
    device->unmapMemory(*uniformBufferMemory);
}

INSTANTIATE_TEST_CASE_P(GfxstreamEnd2EndTests, GfxstreamEnd2EndVkSnapshotBufferTest,
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
