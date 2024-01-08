/*
 * Copyright 2023 Google LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <cutils/log.h>

#include "drm_fourcc.h"
#include "TestingAndroidWsi.h"

namespace gfxstream {

static constexpr int numFds = 0;
static constexpr int numInts = 1;

std::optional<uint32_t> GlFormatToDrmFormat(uint32_t glFormat) {
    switch (glFormat) {
        case kGlRGB:
            return DRM_FORMAT_BGR888;
        case kGlRGB565:
            return DRM_FORMAT_BGR565;
        case kGlRGBA:
            return DRM_FORMAT_ABGR8888;
    }
    return std::nullopt;
}

std::optional<uint32_t> DrmToVirglFormat(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_ABGR8888:
            return VIRGL_FORMAT_B8G8R8A8_UNORM;
        case DRM_FORMAT_BGR888:
            return VIRGL_FORMAT_R8G8B8_UNORM;
        case DRM_FORMAT_BGR565:
            return VIRGL_FORMAT_B5G6R5_UNORM;
    }
    return std::nullopt;
}

TestingAHardwareBuffer::TestingAHardwareBuffer(
        uint32_t width,
        uint32_t height,
        VirtGpuBlobPtr resource)
    : mWidth(width),
      mHeight(height),
      mResource(resource) {
    mHandle = native_handle_create(numFds, numInts);
    mHandle->data[0] = mResource->getResourceHandle();
}

TestingAHardwareBuffer::~TestingAHardwareBuffer() {
    native_handle_close(mHandle);
}

uint32_t TestingAHardwareBuffer::getResourceId() const {
    return mResource->getResourceHandle();
}

uint32_t TestingAHardwareBuffer::getWidth() const {
    return mWidth;
}

uint32_t TestingAHardwareBuffer::getHeight() const {
    return mHeight;
}

int TestingAHardwareBuffer::getAndroidFormat() const {
    return /*AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM=*/1;
}

uint32_t TestingAHardwareBuffer::getDrmFormat() const {
    return DRM_FORMAT_ABGR8888;
}

AHardwareBuffer* TestingAHardwareBuffer::asAHardwareBuffer() {
    return reinterpret_cast<AHardwareBuffer*>(this);
}

buffer_handle_t TestingAHardwareBuffer::asBufferHandle() {
    return reinterpret_cast<buffer_handle_t>(mHandle);
}

EGLClientBuffer TestingAHardwareBuffer::asEglClientBuffer() {
    return reinterpret_cast<EGLClientBuffer>(this);
}

TestingVirtGpuGralloc::TestingVirtGpuGralloc() {}

uint32_t TestingVirtGpuGralloc::createColorBuffer(
        void*,
        int width,
        int height,
        uint32_t glFormat) {
    auto drmFormat = GlFormatToDrmFormat(glFormat);
    if (!drmFormat) {
        ALOGE("Unhandled format");
    }

    auto ahb = allocate(width, height, *drmFormat);

    uint32_t hostHandle = ahb->getResourceId();
    mAllocatedColorBuffers.emplace(hostHandle, std::move(ahb));
    return hostHandle;
}

int TestingVirtGpuGralloc::allocate(
        uint32_t width,
        uint32_t height,
        uint32_t format,
        uint64_t usage,
        AHardwareBuffer** outputAhb) {
    (void)width;
    (void)height;
    (void)format;
    (void)usage;
    (void)outputAhb;

    // TODO: support export flow?
    ALOGE("Unimplemented");

    return 0;
}

std::unique_ptr<TestingAHardwareBuffer> TestingVirtGpuGralloc::allocate(
        uint32_t width,
        uint32_t height,
        uint32_t format) {
    ALOGE("Allocating AHB w:%u, h:%u, format %u", width, height, format);

    auto device = VirtGpuDevice::getInstance();
    if (!device) {
        ALOGE("Failed to allocate: no virtio gpu device.");
        return nullptr;
    }

    auto virglFormat = DrmToVirglFormat(format);
    if (!virglFormat) {
        std::cout << "Unhandled DRM format:" << format;
        return nullptr;
    }

    auto resource = device->createVirglBlob(width, height, *virglFormat);
    if (!resource) {
        ALOGE("Failed to allocate: failed to create virtio resource.");
        return nullptr;
    }

    resource->wait();

    return std::make_unique<TestingAHardwareBuffer>(width, height, std::move(resource));
}

void TestingVirtGpuGralloc::acquire(AHardwareBuffer* ahb) {
    // TODO
}

void TestingVirtGpuGralloc::release(AHardwareBuffer* ahb) {
    // TODO
}

uint32_t TestingVirtGpuGralloc::getHostHandle(const native_handle_t* handle) {
    const auto* ahb = reinterpret_cast<const TestingAHardwareBuffer*>(handle);
    return ahb->getResourceId();
}

uint32_t TestingVirtGpuGralloc::getHostHandle(const AHardwareBuffer* handle) {
    const auto* ahb = reinterpret_cast<const TestingAHardwareBuffer*>(handle);
    return ahb->getResourceId();
}

int TestingVirtGpuGralloc::getFormat(const native_handle_t* handle) {
    const auto* ahb = reinterpret_cast<const TestingAHardwareBuffer*>(handle);
    return ahb->getAndroidFormat();
}

int TestingVirtGpuGralloc::getFormat(const AHardwareBuffer* handle) {
    const auto* ahb = reinterpret_cast<const TestingAHardwareBuffer*>(handle);
    return ahb->getAndroidFormat();
}

uint32_t TestingVirtGpuGralloc::getFormatDrmFourcc(const AHardwareBuffer* handle) {
    const auto* ahb = reinterpret_cast<const TestingAHardwareBuffer*>(handle);
    return ahb->getDrmFormat();
}

size_t TestingVirtGpuGralloc::getAllocatedSize(const native_handle_t*) {
    ALOGE("Unimplemented.");
    return 0;
}

size_t TestingVirtGpuGralloc::getAllocatedSize(const AHardwareBuffer*) {
    ALOGE("Unimplemented.");
    return 0;
}

TestingANativeWindow::TestingANativeWindow(
        uint32_t width,
        uint32_t height,
        uint32_t format,
        std::vector<std::unique_ptr<TestingAHardwareBuffer>> buffers)
    : mWidth(width),
      mHeight(height),
      mFormat(format),
      mBuffers(std::move(buffers)) {
    for (auto& buffer : mBuffers) {
        mBufferQueue.push_back(QueuedAHB{
            .ahb = buffer.get(),
            .fence = -1,
        });
    }
}

EGLNativeWindowType TestingANativeWindow::asEglNativeWindowType() {
    return reinterpret_cast<EGLNativeWindowType>(this);
}

uint32_t TestingANativeWindow::getWidth() const {
    return mWidth;
}

uint32_t TestingANativeWindow::getHeight() const {
    return mHeight;
}

int TestingANativeWindow::getFormat() const {
    return mFormat;
}

int TestingANativeWindow::queueBuffer(EGLClientBuffer buffer, int fence) {
    auto ahb = reinterpret_cast<TestingAHardwareBuffer*>(buffer);

    mBufferQueue.push_back(QueuedAHB{
        .ahb = ahb,
        .fence = fence,
    });

    return 0;
}

int TestingANativeWindow::dequeueBuffer(EGLClientBuffer* buffer, int* fence) {
    auto queuedAhb = mBufferQueue.front();
    mBufferQueue.pop_front();

    *buffer = queuedAhb.ahb->asEglClientBuffer();
    *fence = queuedAhb.fence;
    return 0;
}

int TestingANativeWindow::cancelBuffer(EGLClientBuffer buffer) {
    auto ahb = reinterpret_cast<TestingAHardwareBuffer*>(buffer);

    mBufferQueue.push_back(QueuedAHB{
        .ahb = ahb,
        .fence = -1,
    });

    return 0;
}

bool TestingVirtGpuANativeWindowHelper::isValid(EGLNativeWindowType window) {
    // TODO: maybe a registry of valid TestingANativeWindow-s?
    return true;
}

bool TestingVirtGpuANativeWindowHelper::isValid(EGLClientBuffer buffer) {
    // TODO: maybe a registry of valid TestingAHardwareBuffer-s?
    return true;
}

void TestingVirtGpuANativeWindowHelper::acquire(EGLNativeWindowType window) {
    // TODO: maybe a registry of valid TestingANativeWindow-s?
}

void TestingVirtGpuANativeWindowHelper::release(EGLNativeWindowType window) {
    // TODO: maybe a registry of valid TestingANativeWindow-s?
}

void TestingVirtGpuANativeWindowHelper::acquire(EGLClientBuffer buffer) {
    // TODO: maybe a registry of valid TestingAHardwareBuffer-s?
}

void TestingVirtGpuANativeWindowHelper::release(EGLClientBuffer buffer) {
    // TODO: maybe a registry of valid TestingAHardwareBuffer-s?
}

int TestingVirtGpuANativeWindowHelper::getConsumerUsage(EGLNativeWindowType window, int* usage) {
    (void)window;
    (void)usage;
    return 0;
}
void TestingVirtGpuANativeWindowHelper::setUsage(EGLNativeWindowType window, int usage) {
    (void)window;
    (void)usage;
}

int TestingVirtGpuANativeWindowHelper::getWidth(EGLNativeWindowType window) {
    auto anw = reinterpret_cast<TestingANativeWindow*>(window);
    return anw->getWidth();
}

int TestingVirtGpuANativeWindowHelper::getHeight(EGLNativeWindowType window) {
    auto anw = reinterpret_cast<TestingANativeWindow*>(window);
    return anw->getHeight();
}

int TestingVirtGpuANativeWindowHelper::getWidth(EGLClientBuffer buffer) {
    auto ahb = reinterpret_cast<TestingAHardwareBuffer*>(buffer);
    return ahb->getWidth();
}

int TestingVirtGpuANativeWindowHelper::getHeight(EGLClientBuffer buffer) {
    auto ahb = reinterpret_cast<TestingAHardwareBuffer*>(buffer);
    return ahb->getHeight();
}

int TestingVirtGpuANativeWindowHelper::getFormat(EGLClientBuffer buffer, Gralloc* helper) {
    auto ahb = reinterpret_cast<TestingAHardwareBuffer*>(buffer);
    return ahb->getAndroidFormat();
}

void TestingVirtGpuANativeWindowHelper::setSwapInterval(EGLNativeWindowType window, int interval) {
    ALOGE("Unimplemented");
}

int TestingVirtGpuANativeWindowHelper::queueBuffer(EGLNativeWindowType window, EGLClientBuffer buffer, int fence) {
    auto anw = reinterpret_cast<TestingANativeWindow*>(window);
    return anw->queueBuffer(buffer, fence);
}

int TestingVirtGpuANativeWindowHelper::dequeueBuffer(EGLNativeWindowType window, EGLClientBuffer* buffer, int* fence) {
    auto anw = reinterpret_cast<TestingANativeWindow*>(window);
    return anw->dequeueBuffer(buffer, fence);
}

int TestingVirtGpuANativeWindowHelper::cancelBuffer(EGLNativeWindowType window, EGLClientBuffer buffer) {
    auto anw = reinterpret_cast<TestingANativeWindow*>(window);
    return anw->cancelBuffer(buffer);
}

int TestingVirtGpuANativeWindowHelper::getHostHandle(EGLClientBuffer buffer, Gralloc*) {
    auto ahb = reinterpret_cast<TestingAHardwareBuffer*>(buffer);
    return ahb->getResourceId();
}

}  // namespace gfxstream
