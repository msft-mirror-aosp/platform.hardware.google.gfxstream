ifeq (true,$(GOLDFISH_OPENGL_SHOULD_BUILD))
ifeq ($(shell test $(PLATFORM_SDK_VERSION) -gt 23 && echo isApi24OrHigher),isApi24OrHigher)

LOCAL_PATH := $(call my-dir)

$(call emugl-begin-shared-library,vulkan.ranchu)
$(call emugl-export,C_INCLUDES,$(LOCAL_PATH))
$(call emugl-import,libOpenglSystemCommon)
$(call emugl-import,libOpenglCodecCommon$(GOLDFISH_OPENGL_LIB_SUFFIX))

# Vulkan include dir
ifeq (true,$(GOLDFISH_OPENGL_BUILD_FOR_HOST))
LOCAL_C_INCLUDES += $(HOST_EMUGL_PATH)/host/include
endif

ifneq (true,$(GOLDFISH_OPENGL_BUILD_FOR_HOST))
LOCAL_HEADER_LIBRARIES += \
    vulkan_headers \

endif

LOCAL_CFLAGS += \
    -DLOG_TAG=\"goldfish_vulkan\" \
    -Wno-missing-field-initializers \
    -fvisibility=hidden \
    -fstrict-aliasing \
    -DVK_USE_PLATFORM_ANDROID_KHR \
    -DVK_NO_PROTOTYPES \

LOCAL_SRC_FILES := goldfish_vulkan.cpp

$(call emugl-end-module)

endif # API 24 or later
endif # GOLDFISH_OPENGL_SHOULD_BUILD
