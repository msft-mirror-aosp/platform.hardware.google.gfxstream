/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GraphicsDetectorGl.h"

#include "Egl.h"
#include "Gles.h"

namespace gfxstream {
namespace {

constexpr const char kSurfacelessContextExt[] = "EGL_KHR_surfaceless_context";

class Closer {
  public:
    Closer(std::function<void()> on_close) : on_close_(std::move(on_close)) {}
    ~Closer() { on_close_(); }

  private:
    std::function<void()> on_close_;
};

}  // namespace

gfxstream::expected<Ok, std::string> PopulateEglAndGlesAvailability(
        ::gfxstream::proto::GraphicsAvailability* availability) {
    auto egl = GFXSTREAM_EXPECT(Egl::Load());

    ::gfxstream::proto::EglAvailability* eglAvailability = availability->mutable_egl();

    EGLDisplay display = egl.eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        if (egl.eglGetPlatformDisplayEXT != nullptr) {
            display = egl.eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        }
    }

    if (display == EGL_NO_DISPLAY) {
        return gfxstream::unexpected("Failed to find display.");
    }

    EGLint client_version_major = 0;
    EGLint client_version_minor = 0;
    if (egl.eglInitialize(display, &client_version_major,
                            &client_version_minor) != EGL_TRUE) {
        return gfxstream::unexpected("Failed to initialize display.");
    }

    const std::string version_string = egl.eglQueryString(display, EGL_VERSION);
    if (version_string.empty()) {
        return gfxstream::unexpected("Failed to query client version.");
    }
    eglAvailability->set_version(version_string);

    const std::string vendor_string = egl.eglQueryString(display, EGL_VENDOR);
    if (vendor_string.empty()) {
        return gfxstream::unexpected("Failed to query vendor.");
    }
    eglAvailability->set_vendor(vendor_string);

    const std::string extensions_string =
        egl.eglQueryString(display, EGL_EXTENSIONS);
    if (extensions_string.empty()) {
        return gfxstream::unexpected("Failed to query extensions.");
    }
    eglAvailability->set_extensions(extensions_string);

    if (extensions_string.find(kSurfacelessContextExt) == std::string::npos) {
        return gfxstream::unexpected("Failed to find extension EGL_KHR_surfaceless_context.");
    }

    const std::string display_apis_string = egl.eglQueryString(display, EGL_CLIENT_APIS);
    if (display_apis_string.empty()) {
        return gfxstream::unexpected("Failed to query display apis.");
    }

    if (egl.eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
        return gfxstream::unexpected("Failed to bind GLES API.");
    }

    const EGLint framebuffer_config_attributes[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        1,
        EGL_GREEN_SIZE,      1,
        EGL_BLUE_SIZE,       1,
        EGL_ALPHA_SIZE,      0,
        EGL_NONE,
    };

    EGLConfig framebuffer_config;
    EGLint num_framebuffer_configs = 0;
    if (egl.eglChooseConfig(display, framebuffer_config_attributes, &framebuffer_config, 1, &num_framebuffer_configs) != EGL_TRUE) {
        return gfxstream::unexpected("Failed to find matching framebuffer config.");
    }


    const EGLint gles2_context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    EGLContext gles2_context = egl.eglCreateContext(display, framebuffer_config, EGL_NO_CONTEXT, gles2_context_attributes);
    if (gles2_context != EGL_NO_CONTEXT) {
        Closer context_closer(
            [&]() { egl.eglDestroyContext(display, gles2_context); });

        if (egl.eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, gles2_context) != EGL_TRUE) {
            return gfxstream::unexpected("Failed to make GLES2 context current.");
        }

        auto *gles2Availability = eglAvailability->mutable_gles2_availability();

        auto gles = GFXSTREAM_EXPECT(Gles::LoadFromEgl(&egl));

        const GLubyte* gles2_vendor = gles.glGetString(GL_VENDOR);
        if (gles2_vendor == nullptr) {
            return gfxstream::unexpected("Failed to query GLES2 vendor.");
        }
        const std::string gles2_vendor_string((const char*)gles2_vendor);
        gles2Availability->set_vendor(gles2_vendor_string);

        const GLubyte* gles2_version = gles.glGetString(GL_VERSION);
        if (gles2_version == nullptr) {
             gfxstream::unexpected("Failed to query GLES2 vendor.");
        }
        const std::string gles2_version_string((const char*)gles2_version);
        gles2Availability->set_version(gles2_version_string);

        const GLubyte* gles2_renderer = gles.glGetString(GL_RENDERER);
        if (gles2_renderer == nullptr) {
             gfxstream::unexpected("Failed to query GLES2 renderer.");
        }
        const std::string gles2_renderer_string((const char*)gles2_renderer);
        gles2Availability->set_renderer(gles2_renderer_string);

        const GLubyte* gles2_extensions = gles.glGetString(GL_EXTENSIONS);
        if (gles2_extensions == nullptr) {
            return gfxstream::unexpected("Failed to query GLES2 extensions.");
        }
        const std::string gles2_extensions_string((const char*)gles2_extensions);
        gles2Availability->set_extensions(gles2_extensions_string);
    }

    const EGLint gles3_context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE,
    };
    EGLContext gles3_context = egl.eglCreateContext(display, framebuffer_config, EGL_NO_CONTEXT, gles3_context_attributes);
    if (gles3_context != EGL_NO_CONTEXT) {
        Closer context_closer([&]() { egl.eglDestroyContext(display, gles3_context); });

        if (egl.eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, gles3_context) != EGL_TRUE) {
            return gfxstream::unexpected("Failed to make GLES3 context current.");
        }
        auto *gles3Availability = eglAvailability->mutable_gles3_availability();

        auto gles = GFXSTREAM_EXPECT(Gles::LoadFromEgl(&egl));

        const GLubyte* gles3_vendor = gles.glGetString(GL_VENDOR);
        if (gles3_vendor == nullptr) {
            gfxstream::unexpected("Failed to query GLES3 vendor.");
        }
        const std::string gles3_vendor_string((const char*)gles3_vendor);
        gles3Availability->set_vendor(gles3_vendor_string);

        const GLubyte* gles3_version = gles.glGetString(GL_VERSION);
        if (gles3_version == nullptr) {
            gfxstream::unexpected("Failed to query GLES2 vendor.");
        }
        const std::string gles3_version_string((const char*)gles3_version);
        gles3Availability->set_version(gles3_version_string);

        const GLubyte* gles3_renderer = gles.glGetString(GL_RENDERER);
        if (gles3_renderer == nullptr) {
            return gfxstream::unexpected("Failed to query GLES3 renderer.");
        }
        const std::string gles3_renderer_string((const char*)gles3_renderer);
        gles3Availability->set_renderer(gles3_renderer_string);

        const GLubyte* gles3_extensions = gles.glGetString(GL_EXTENSIONS);
        if (gles3_extensions == nullptr) {
            return gfxstream::unexpected("Failed to query GLES3 extensions.");
        }
        const std::string gles3_extensions_string((const char*)gles3_extensions);
        gles3Availability->set_extensions(gles3_extensions_string);
    }

    return Ok{};
}

}  // namespace gfxstream
