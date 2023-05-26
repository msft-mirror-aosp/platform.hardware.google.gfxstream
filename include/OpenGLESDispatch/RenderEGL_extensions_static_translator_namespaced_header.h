// Auto-generated with: android/scripts/gen-entries.py --mode=static_translator_namespaced_header stream-servers/gl/OpenGLESDispatch/render_egl_extensions.entries --output=include/OpenGLESDispatch/RenderEGL_extensions_static_translator_namespaced_header.h
// DO NOT EDIT THIS FILE

#pragma once

#include <EGL/egl.h>
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/eglext.h>
namespace translator {
namespace egl {
EGLAPI EGLImageKHR EGLAPIENTRY eglCreateImageKHR(EGLDisplay display, EGLContext context, EGLenum target, EGLClientBuffer buffer, const EGLint* attrib_list);
EGLAPI EGLBoolean EGLAPIENTRY eglDestroyImageKHR(EGLDisplay display, EGLImageKHR image);
EGLAPI EGLSyncKHR EGLAPIENTRY eglCreateSyncKHR(EGLDisplay display, EGLenum type, const EGLint* attribs);
EGLAPI EGLint EGLAPIENTRY eglClientWaitSyncKHR(EGLDisplay display, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout);
EGLAPI EGLint EGLAPIENTRY eglWaitSyncKHR(EGLDisplay display, EGLSyncKHR sync, EGLint flags);
EGLAPI EGLBoolean EGLAPIENTRY eglDestroySyncKHR(EGLDisplay display, EGLSyncKHR sync);
EGLAPI EGLint EGLAPIENTRY eglGetMaxGLESVersion(EGLDisplay display);
EGLAPI void EGLAPIENTRY eglBlitFromCurrentReadBufferANDROID(EGLDisplay display, EGLImageKHR image);
EGLAPI void* EGLAPIENTRY eglSetImageFenceANDROID(EGLDisplay display, EGLImageKHR image);
EGLAPI void EGLAPIENTRY eglWaitImageFenceANDROID(EGLDisplay display, void* fence);
EGLAPI void EGLAPIENTRY eglAddLibrarySearchPathANDROID(const char* path);
EGLAPI EGLBoolean EGLAPIENTRY eglQueryVulkanInteropSupportANDROID();
EGLAPI EGLBoolean EGLAPIENTRY eglGetSyncAttribKHR(EGLDisplay display, EGLSync sync, EGLint attribute, EGLint * value);
EGLAPI EGLDisplay EGLAPIENTRY eglGetNativeDisplayANDROID(EGLDisplay display);
EGLAPI EGLContext EGLAPIENTRY eglGetNativeContextANDROID(EGLDisplay display, EGLContext context);
EGLAPI EGLImage EGLAPIENTRY eglGetNativeImageANDROID(EGLDisplay display, EGLImage image);
EGLAPI EGLBoolean EGLAPIENTRY eglSetImageInfoANDROID(EGLDisplay display, EGLImage image, EGLint width, EGLint height, EGLint internalformat);
EGLAPI EGLImage EGLAPIENTRY eglImportImageANDROID(EGLDisplay display, EGLImage image);
EGLAPI EGLint EGLAPIENTRY eglDebugMessageControlKHR(EGLDEBUGPROCKHR callback, const EGLAttrib * attrib_list);
} // namespace translator
} // namespace egl
