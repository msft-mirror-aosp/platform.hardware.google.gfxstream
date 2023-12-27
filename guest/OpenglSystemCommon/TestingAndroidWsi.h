/*
 * Copyright 2023 Google LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

#include "VirtGpu.h"
#include "ANativeWindow.h"

namespace gfxstream {

class TestingAHardwareBuffer {
  public:
    TestingAHardwareBuffer(uint32_t width,
                           uint32_t height,
                           VirtGpuBlobPtr resource);

    ~TestingAHardwareBuffer();

    uint32_t getResourceId() const;

    uint32_t getWidth() const;

    uint32_t getHeight() const;

    int getAndroidFormat() const;

    uint32_t getDrmFormat() const;

    AHardwareBuffer* asAHardwareBuffer();

    buffer_handle_t asBufferHandle();

    EGLClientBuffer asEglClientBuffer();

  private:
    uint32_t mWidth;
    uint32_t mHeight;
    VirtGpuBlobPtr mResource;
    native_handle_t *mHandle = nullptr;
};

class TestingVirtGpuGralloc : public Gralloc {
   public:
    TestingVirtGpuGralloc();

    uint32_t createColorBuffer(void*,
                               int width,
                               int height,
                               uint32_t glFormat) override;

    int allocate(uint32_t width,
                 uint32_t height,
                 uint32_t format,
                 uint64_t usage,
                 AHardwareBuffer** outputAhb) override;

    std::unique_ptr<TestingAHardwareBuffer> allocate(uint32_t width,
                                                     uint32_t height,
                                                     uint32_t format);

    void acquire(AHardwareBuffer* ahb) override;
    void release(AHardwareBuffer* ahb) override;

    uint32_t getHostHandle(const native_handle_t* handle) override;
    uint32_t getHostHandle(const AHardwareBuffer* handle) override;

    int getFormat(const native_handle_t* handle) override;
    int getFormat(const AHardwareBuffer* handle) override;

    uint32_t getFormatDrmFourcc(const AHardwareBuffer* handle) override;

    size_t getAllocatedSize(const native_handle_t*) override;
    size_t getAllocatedSize(const AHardwareBuffer*) override;

  private:
    std::unordered_map<uint32_t, std::unique_ptr<TestingAHardwareBuffer>> mAllocatedColorBuffers;
};

class TestingANativeWindow {
  public:
    TestingANativeWindow(uint32_t width,
                         uint32_t height,
                         uint32_t format,
                         std::vector<std::unique_ptr<TestingAHardwareBuffer>> buffers);

    EGLNativeWindowType asEglNativeWindowType();

    uint32_t getWidth() const;

    uint32_t getHeight() const;

    int getFormat() const;

    int queueBuffer(EGLClientBuffer buffer, int fence);
    int dequeueBuffer(EGLClientBuffer* buffer, int* fence);
    int cancelBuffer(EGLClientBuffer buffer);

  private:
    uint32_t mWidth;
    uint32_t mHeight;
    uint32_t mFormat;
    std::vector<std::unique_ptr<TestingAHardwareBuffer>> mBuffers;

    struct QueuedAHB {
        TestingAHardwareBuffer* ahb;
        int fence = -1;
    };
    std::deque<QueuedAHB> mBufferQueue;
};

class TestingVirtGpuANativeWindowHelper : public ANativeWindowHelper {
  public:
    bool isValid(EGLNativeWindowType window) override;
    bool isValid(EGLClientBuffer buffer) override;

    void acquire(EGLNativeWindowType window) override;
    void release(EGLNativeWindowType window) override;

    void acquire(EGLClientBuffer buffer) override;
    void release(EGLClientBuffer buffer) override;

    int getConsumerUsage(EGLNativeWindowType window, int* usage) override;
    void setUsage(EGLNativeWindowType window, int usage) override;

    int getWidth(EGLNativeWindowType window) override;
    int getHeight(EGLNativeWindowType window) override;

    int getWidth(EGLClientBuffer buffer) override;
    int getHeight(EGLClientBuffer buffer) override;

    int getFormat(EGLClientBuffer buffer, Gralloc* helper) override;

    void setSwapInterval(EGLNativeWindowType window, int interval) override;

    int queueBuffer(EGLNativeWindowType window, EGLClientBuffer buffer, int fence) override;
    int dequeueBuffer(EGLNativeWindowType window, EGLClientBuffer* buffer, int* fence) override;
    int cancelBuffer(EGLNativeWindowType window, EGLClientBuffer buffer) override;

    int getHostHandle(EGLClientBuffer buffer, Gralloc*) override;
};

}  // namespace gfxstream
