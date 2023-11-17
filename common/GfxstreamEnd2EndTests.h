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

#include <future>
#include <inttypes.h>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

// clang-format off
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "OpenGLESDispatch/gldefs.h"
#include "OpenGLESDispatch/gles_functions.h"
#include "OpenGLESDispatch/RenderEGL_functions.h"
#include "OpenGLESDispatch/RenderEGL_extensions_functions.h"

#define VULKAN_HPP_NAMESPACE vkhpp
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 1
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_EXCEPTIONS
#include <vulkan/vulkan.hpp>
#include <vulkan/vk_android_native_buffer.h>

// clang-format on

#include <android-base/expected.h>

#include "HostConnection.h"

namespace gfxstream {
namespace tests {

MATCHER(IsOk, "an ok result") {
  auto& result = arg;
  if (!result.ok()) {
    *result_listener << "which is an error with message: \""
                     << result.error()
                     << "\"";
    return false;
  }
  return true;
}

MATCHER(IsError, "an error result") {
    auto& result = arg;
    if (result.ok()) {
        *result_listener << "which is an ok result";
        return false;
    }
    return true;
}

MATCHER(IsVkSuccess, "is VK_SUCCESS") {
    auto& result = arg;
    if (result != vkhpp::Result::eSuccess) {
        *result_listener << "which is " << vkhpp::to_string(result);
        return false;
    }
    return true;
}

MATCHER(IsValidHandle, "a non-null handle") {
    auto& result = arg;
    if (!result) {
        *result_listener << "which is a VK_NULL_HANDLE";
        return false;
    }
    return true;
}

template <typename GlType>
using GlExpected = android::base::expected<GlType, std::string>;

template <typename VkType>
using VkExpected = android::base::expected<VkType, vkhpp::Result>;

#define VK_ASSERT(x)                                                          \
  ({                                                                          \
    auto vk_expect_android_base_expected = (x);                               \
    if (!vk_expect_android_base_expected.ok()) {                              \
      ASSERT_THAT(vk_expect_android_base_expected.ok(), ::testing::IsTrue()); \
    };                                                                        \
    std::move(vk_expect_android_base_expected.value());                       \
  })

#define VK_ASSERT_RV(x)                                                       \
  ({                                                                          \
    auto vkhpp_result_value = (x);                                            \
    ASSERT_THAT(vkhpp_result_value.result, IsVkSuccess());                    \
    std::move(vkhpp_result_value.value);                                      \
  })

#define VK_EXPECT_RESULT(x)                                                   \
  ({                                                                          \
    auto vkhpp_result = (x);                                                  \
    if (vkhpp_result != vkhpp::Result::eSuccess) {                            \
        return android::base::unexpected(vkhpp_result);                       \
    }                                                                         \
  })

#define VK_EXPECT_RV(x)                                                       \
  ({                                                                          \
    auto vkhpp_result_value = (x);                                            \
    if (vkhpp_result_value.result != vkhpp::Result::eSuccess) {               \
        return android::base::unexpected(vkhpp_result_value.result);          \
    }                                                                         \
    std::move(vkhpp_result_value.value);                                      \
  })

#define VK_TRY(x)                                                             \
  ({                                                                          \
    auto vkhpp_result = (x);                                                  \
    if (vkhpp_result != vkhpp::Result::eSuccess) {                            \
        return vkhpp_result;                                                  \
    }                                                                         \
  })

#define VK_TRY_RV(x)                                                          \
  ({                                                                          \
    auto vkhpp_result_value = (x);                                            \
    if (vkhpp_result_value.result != vkhpp::Result::eSuccess) {               \
        return vkhpp_result_value.result;                                     \
    }                                                                         \
    std::move(vkhpp_result_value.value);                                      \
  })

class TestingAHardwareBuffer {
  public:
    TestingAHardwareBuffer(uint32_t width,
                           uint32_t height,
                           VirtGpuBlobPtr resource);

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

struct TestParams {
    bool with_gl;
    bool with_vk;
    bool with_vk_snapshot = false;

    std::string ToString() const;
    friend std::ostream& operator<<(std::ostream& os, const TestParams& params);
};

std::string GetTestName(const ::testing::TestParamInfo<TestParams>& info);

class GfxstreamEnd2EndTest : public ::testing::TestWithParam<TestParams> {
  protected:
    struct GuestGlDispatchTable {
        #define DECLARE_EGL_FUNCTION(return_type, function_name, signature) \
            return_type (*function_name) signature = nullptr;

        #define DECLARE_GLES_FUNCTION(return_type, function_name, signature, args) \
            return_type (*function_name) signature = nullptr;

        LIST_RENDER_EGL_FUNCTIONS(DECLARE_EGL_FUNCTION)
        LIST_RENDER_EGL_EXTENSIONS_FUNCTIONS(DECLARE_EGL_FUNCTION)
        LIST_GLES_FUNCTIONS(DECLARE_GLES_FUNCTION, DECLARE_GLES_FUNCTION)
    };

    std::unique_ptr<GuestGlDispatchTable> SetupGuestGl();
    std::unique_ptr<vkhpp::DynamicLoader> SetupGuestVk();

    void SetUp() override;

    void TearDownGuest();
    void TearDownHost();
    void TearDown() override;

    std::unique_ptr<TestingANativeWindow> CreateEmulatedANW(uint32_t width, uint32_t height);

    void SetUpEglContextAndSurface(uint32_t contextVersion,
                                   uint32_t width,
                                   uint32_t height,
                                   EGLDisplay* outDisplay,
                                   EGLContext* outContext,
                                   EGLSurface* outSurface);

    void TearDownEglContextAndSurface(EGLDisplay display,
                                      EGLContext context,
                                      EGLSurface surface);

    GlExpected<GLuint> SetUpShader(GLenum type, const std::string& source);

    GlExpected<GLuint> SetUpProgram(const std::string& vertSource,
                                    const std::string& fragSource);

    struct TypicalVkTestEnvironment {
        vkhpp::UniqueInstance instance;
        vkhpp::PhysicalDevice physicalDevice;
        vkhpp::UniqueDevice device;
        vkhpp::Queue queue;
        uint32_t queueFamilyIndex;
    };
    VkExpected<TypicalVkTestEnvironment> SetUpTypicalVkTestEnvironment(
        uint32_t apiVersion = VK_API_VERSION_1_2);

    std::unique_ptr<TestingVirtGpuANativeWindowHelper> mAnwHelper;
    std::unique_ptr<TestingVirtGpuGralloc> mGralloc;
    SyncHelper* mSync;
    std::unique_ptr<GuestGlDispatchTable> mGl;
    std::unique_ptr<vkhpp::DynamicLoader> mVk;
};

}  // namespace tests
}  // namespace gfxstream
