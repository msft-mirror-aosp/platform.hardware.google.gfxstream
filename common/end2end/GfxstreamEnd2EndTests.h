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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <android-base/expected.h>
#include <inttypes.h>

#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <variant>

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

#include "Sync.h"
#include "drm_fourcc.h"
#include "gfxstream/guest/ANativeWindow.h"
#include "gfxstream/guest/Gralloc.h"
#include "gfxstream/guest/RenderControlApi.h"

namespace gfxstream {
namespace tests {

constexpr const bool kSaveImagesIfComparisonFailed = false;

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

struct Ok {};

template <typename GlType>
using GlExpected = android::base::expected<GlType, std::string>;

#define GL_ASSERT(x)                        \
    ({                                      \
        auto gl_result = (x);               \
        if (!gl_result.ok()) {              \
            ASSERT_THAT(gl_result, IsOk()); \
        }                                   \
        std::move(gl_result.value());       \
    })

#define GL_EXPECT(x)                                             \
    ({                                                           \
        auto gl_result = (x);                                    \
        if (!gl_result.ok()) {                                   \
            return android::base::unexpected(gl_result.error()); \
        }                                                        \
        std::move(gl_result.value());                            \
    })

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

#define VK_TRY(x)                                                                   \
    ({                                                                              \
        auto vk_try_android_base_expected = (x);                                    \
        if (!vk_try_android_base_expected.ok()) {                                   \
            return android::base::unexpected(vk_try_android_base_expected.error()); \
        }                                                                           \
        std::move(vk_try_android_base_expected.value());                            \
    })

#define VK_TRY_RESULT(x)                               \
    ({                                                 \
        auto vkhpp_result = (x);                       \
        if (vkhpp_result != vkhpp::Result::eSuccess) { \
            return vkhpp_result;                       \
        }                                              \
    })

#define VK_TRY_RV(x)                                                          \
  ({                                                                          \
    auto vkhpp_result_value = (x);                                            \
    if (vkhpp_result_value.result != vkhpp::Result::eSuccess) {               \
        return vkhpp_result_value.result;                                     \
    }                                                                         \
    std::move(vkhpp_result_value.value);                                      \
  })

struct GuestGlDispatchTable {
#define DECLARE_EGL_FUNCTION(return_type, function_name, signature) \
    return_type(*function_name) signature = nullptr;

#define DECLARE_GLES_FUNCTION(return_type, function_name, signature, args) \
    return_type(*function_name) signature = nullptr;

    LIST_RENDER_EGL_FUNCTIONS(DECLARE_EGL_FUNCTION)
    LIST_RENDER_EGL_EXTENSIONS_FUNCTIONS(DECLARE_EGL_FUNCTION)
    LIST_GLES_FUNCTIONS(DECLARE_GLES_FUNCTION, DECLARE_GLES_FUNCTION)
};

struct GuestRenderControlDispatchTable {
    PFN_rcCreateDevice rcCreateDevice = nullptr;
    PFN_rcDestroyDevice rcDestroyDevice = nullptr;
    PFN_rcCompose rcCompose = nullptr;
};

class ScopedRenderControlDevice {
   public:
    ScopedRenderControlDevice() {}

    ScopedRenderControlDevice(GuestRenderControlDispatchTable& dispatch) : mDispatch(&dispatch) {
        mDevice = dispatch.rcCreateDevice();
    }

    ScopedRenderControlDevice(const ScopedRenderControlDevice& rhs) = delete;
    ScopedRenderControlDevice& operator=(const ScopedRenderControlDevice& rhs) = delete;

    ScopedRenderControlDevice(ScopedRenderControlDevice&& rhs)
        : mDispatch(rhs.mDispatch), mDevice(rhs.mDevice) {
        rhs.mDevice = nullptr;
    }

    ScopedRenderControlDevice& operator=(ScopedRenderControlDevice&& rhs) {
        mDispatch = rhs.mDispatch;
        std::swap(mDevice, rhs.mDevice);
        return *this;
    }

    ~ScopedRenderControlDevice() {
        if (mDevice != nullptr) {
            mDispatch->rcDestroyDevice(mDevice);
            mDevice = nullptr;
        }
    }

    operator RenderControlDevice*() { return mDevice; }
    operator RenderControlDevice*() const { return mDevice; }

   private:
    GuestRenderControlDispatchTable* mDispatch = nullptr;
    RenderControlDevice* mDevice = nullptr;
};

class ScopedGlType {
   public:
    using GlDispatch = GuestGlDispatchTable;
    using GlDispatchGenFunc = void (*GuestGlDispatchTable::*)(GLsizei, GLuint*);
    using GlDispatchDelFunc = void (*GuestGlDispatchTable::*)(GLsizei, const GLuint*);

    ScopedGlType() {}

    ScopedGlType(GlDispatch& glDispatch, GlDispatchGenFunc glGenFunc, GlDispatchDelFunc glDelFunc)
        : mGlDispatch(&glDispatch), mGlGenFunc(glGenFunc), mGlDelFunc(glDelFunc) {
        (mGlDispatch->*mGlGenFunc)(1, &mHandle);
    }

    ScopedGlType(const ScopedGlType& rhs) = delete;
    ScopedGlType& operator=(const ScopedGlType& rhs) = delete;

    ScopedGlType(ScopedGlType&& rhs)
        : mGlDispatch(rhs.mGlDispatch),
          mGlGenFunc(rhs.mGlGenFunc),
          mGlDelFunc(rhs.mGlDelFunc),
          mHandle(rhs.mHandle) {
        rhs.mHandle = 0;
    }

    ScopedGlType& operator=(ScopedGlType&& rhs) {
        mGlDispatch = rhs.mGlDispatch;
        mGlGenFunc = rhs.mGlGenFunc;
        mGlDelFunc = rhs.mGlDelFunc;
        std::swap(mHandle, rhs.mHandle);
        return *this;
    }

    ~ScopedGlType() {
        if (mHandle != 0) {
            (mGlDispatch->*mGlDelFunc)(1, &mHandle);
            mHandle = 0;
        }
    }

    operator GLuint() { return mHandle; }
    operator GLuint() const { return mHandle; }

   private:
    GlDispatch* mGlDispatch = nullptr;
    GlDispatchGenFunc mGlGenFunc = nullptr;
    GlDispatchDelFunc mGlDelFunc = nullptr;
    GLuint mHandle = 0;
};

class ScopedGlBuffer : public ScopedGlType {
   public:
    ScopedGlBuffer(GlDispatch& dispatch)
        : ScopedGlType(dispatch, &GlDispatch::glGenBuffers, &GlDispatch::glDeleteBuffers) {}
};

class ScopedGlTexture : public ScopedGlType {
   public:
    ScopedGlTexture(GlDispatch& dispatch)
        : ScopedGlType(dispatch, &GlDispatch::glGenTextures, &GlDispatch::glDeleteTextures) {}
};

class ScopedGlFramebuffer : public ScopedGlType {
   public:
    ScopedGlFramebuffer(GlDispatch& dispatch)
        : ScopedGlType(dispatch, &GlDispatch::glGenFramebuffers,
                       &GlDispatch::glDeleteFramebuffers) {}
};

class ScopedGlShader {
   public:
    using GlDispatch = GuestGlDispatchTable;

    ScopedGlShader() = default;

    ScopedGlShader(const ScopedGlShader& rhs) = delete;
    ScopedGlShader& operator=(const ScopedGlShader& rhs) = delete;

    static GlExpected<ScopedGlShader> MakeShader(GlDispatch& dispatch, GLenum type,
                                                 const std::string& source);

    ScopedGlShader(ScopedGlShader&& rhs) : mGlDispatch(rhs.mGlDispatch), mHandle(rhs.mHandle) {
        rhs.mHandle = 0;
    }

    ScopedGlShader& operator=(ScopedGlShader&& rhs) {
        mGlDispatch = rhs.mGlDispatch;
        std::swap(mHandle, rhs.mHandle);
        return *this;
    }

    ~ScopedGlShader() {
        if (mHandle != 0) {
            mGlDispatch->glDeleteShader(mHandle);
            mHandle = 0;
        }
    }

    operator GLuint() { return mHandle; }
    operator GLuint() const { return mHandle; }

   private:
    ScopedGlShader(GlDispatch& dispatch, GLuint handle) : mGlDispatch(&dispatch), mHandle(handle) {}

    GlDispatch* mGlDispatch = nullptr;
    GLuint mHandle = 0;
};

class ScopedGlProgram {
   public:
    using GlDispatch = GuestGlDispatchTable;

    ScopedGlProgram() = default;

    ScopedGlProgram(const ScopedGlProgram& rhs) = delete;
    ScopedGlProgram& operator=(const ScopedGlProgram& rhs) = delete;

    static GlExpected<ScopedGlProgram> MakeProgram(GlDispatch& dispatch,
                                                   const std::string& vertShader,
                                                   const std::string& fragShader);

    static GlExpected<ScopedGlProgram> MakeProgram(GlDispatch& dispatch, GLenum programBinaryFormat,
                                                   const std::vector<uint8_t>& programBinaryData);

    ScopedGlProgram(ScopedGlProgram&& rhs) : mGlDispatch(rhs.mGlDispatch), mHandle(rhs.mHandle) {
        rhs.mHandle = 0;
    }

    ScopedGlProgram& operator=(ScopedGlProgram&& rhs) {
        mGlDispatch = rhs.mGlDispatch;
        std::swap(mHandle, rhs.mHandle);
        return *this;
    }

    ~ScopedGlProgram() {
        if (mHandle != 0) {
            mGlDispatch->glDeleteProgram(mHandle);
            mHandle = 0;
        }
    }

    operator GLuint() { return mHandle; }
    operator GLuint() const { return mHandle; }

   private:
    ScopedGlProgram(GlDispatch& dispatch, GLuint handle)
        : mGlDispatch(&dispatch), mHandle(handle) {}

    GlDispatch* mGlDispatch = nullptr;
    GLuint mHandle = 0;
};

class ScopedAHardwareBuffer {
   public:
    ScopedAHardwareBuffer() = default;

    static GlExpected<ScopedAHardwareBuffer> Allocate(Gralloc& gralloc, uint32_t width,
                                                      uint32_t height, uint32_t format);

    ScopedAHardwareBuffer(const ScopedAHardwareBuffer& rhs) = delete;
    ScopedAHardwareBuffer& operator=(const ScopedAHardwareBuffer& rhs) = delete;

    ScopedAHardwareBuffer(ScopedAHardwareBuffer&& rhs)
        : mGralloc(rhs.mGralloc), mHandle(rhs.mHandle) {
        rhs.mHandle = nullptr;
    }

    ScopedAHardwareBuffer& operator=(ScopedAHardwareBuffer&& rhs) {
        mGralloc = rhs.mGralloc;
        std::swap(mHandle, rhs.mHandle);
        return *this;
    }

    ~ScopedAHardwareBuffer() {
        if (mHandle != nullptr) {
            mGralloc->release(mHandle);
            mHandle = 0;
        }
    }

    uint32_t GetWidth() const { return mGralloc->getWidth(mHandle); }

    uint32_t GetHeight() const { return mGralloc->getHeight(mHandle); }

    GlExpected<uint8_t*> Lock() {
        uint8_t* mapped = nullptr;
        int status = mGralloc->lock(mHandle, &mapped);
        if (status != 0) {
            return android::base::unexpected("Failed to lock AHB");
        }
        return mapped;
    }

    void Unlock() { mGralloc->unlock(mHandle); }

    operator AHardwareBuffer*() { return mHandle; }
    operator AHardwareBuffer*() const { return mHandle; }

   private:
    ScopedAHardwareBuffer(Gralloc& gralloc, AHardwareBuffer* handle)
        : mGralloc(&gralloc), mHandle(handle) {}

    Gralloc* mGralloc = nullptr;
    AHardwareBuffer* mHandle = nullptr;
};

struct Image {
    uint32_t width;
    uint32_t height;
    std::vector<uint32_t> pixels;
};

enum class GfxstreamTransport {
  kVirtioGpuAsg,
  kVirtioGpuPipe,
};

struct TestParams {
    bool with_gl;
    bool with_vk;
    int samples = 1;
    std::unordered_set<std::string> with_features;
    GfxstreamTransport with_transport = GfxstreamTransport::kVirtioGpuAsg;

    std::string ToString() const;
    friend std::ostream& operator<<(std::ostream& os, const TestParams& params);
};

std::string GetTestName(const ::testing::TestParamInfo<TestParams>& info);

// Generates the cartesian product of params with and without the given features.
std::vector<TestParams> WithAndWithoutFeatures(const std::vector<TestParams>& params,
                                               const std::vector<std::string>& features);

class GfxstreamEnd2EndTest : public ::testing::TestWithParam<TestParams> {
   public:
    std::unique_ptr<GuestGlDispatchTable> SetupGuestGl();
    std::unique_ptr<GuestRenderControlDispatchTable> SetupGuestRc();
    std::unique_ptr<vkhpp::DynamicLoader> SetupGuestVk();

    void SetUp() override;

    void TearDownGuest();
    void TearDownHost();
    void TearDown() override;

    void SetUpEglContextAndSurface(uint32_t contextVersion,
                                   uint32_t width,
                                   uint32_t height,
                                   EGLDisplay* outDisplay,
                                   EGLContext* outContext,
                                   EGLSurface* outSurface);

    void TearDownEglContextAndSurface(EGLDisplay display,
                                      EGLContext context,
                                      EGLSurface surface);

    GlExpected<ScopedGlShader> SetUpShader(GLenum type, const std::string& source);

    GlExpected<ScopedGlProgram> SetUpProgram(const std::string& vertSource,
                                             const std::string& fragSource);

    GlExpected<ScopedGlProgram> SetUpProgram(GLenum programBinaryFormat,
                                             const std::vector<uint8_t>& programBinaryData);

    struct TypicalVkTestEnvironment {
        vkhpp::UniqueInstance instance;
        vkhpp::PhysicalDevice physicalDevice;
        vkhpp::UniqueDevice device;
        vkhpp::Queue queue;
        uint32_t queueFamilyIndex;
    };
    VkExpected<TypicalVkTestEnvironment> SetUpTypicalVkTestEnvironment(
        uint32_t apiVersion = VK_API_VERSION_1_2);

    uint32_t GetMemoryType(const vkhpp::PhysicalDevice& physicalDevice,
                           const vkhpp::MemoryRequirements& memoryRequirements,
                           vkhpp::MemoryPropertyFlags memoryProperties);

    void SnapshotSaveAndLoad();

    GlExpected<Image> LoadImage(const std::string& basename);

    GlExpected<Image> AsImage(ScopedAHardwareBuffer& ahb);

    GlExpected<ScopedAHardwareBuffer> CreateAHBFromImage(const std::string& basename);

    bool ArePixelsSimilar(uint32_t expectedPixel, uint32_t actualPixel);

    bool AreImagesSimilar(const Image& expected, const Image& actual);

    GlExpected<Ok> CompareAHBWithGolden(ScopedAHardwareBuffer& ahb,
                                        const std::string& goldenBasename);

    std::unique_ptr<ANativeWindowHelper> mAnwHelper;
    std::unique_ptr<Gralloc> mGralloc;
    std::unique_ptr<SyncHelper> mSync;
    std::unique_ptr<GuestGlDispatchTable> mGl;
    std::unique_ptr<GuestRenderControlDispatchTable> mRc;
    std::unique_ptr<vkhpp::DynamicLoader> mVk;
};

}  // namespace tests
}  // namespace gfxstream
