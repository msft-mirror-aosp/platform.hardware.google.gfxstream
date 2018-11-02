LOCAL_PATH := $(call my-dir)

### Vulkan Encoder ###########################################
$(call emugl-begin-shared-library,libvulkan_enc)
$(call emugl-import,libvulkan_cereal_guest)

ifneq (true,$(GOLDFISH_OPENGL_BUILD_FOR_HOST))

# Only import androidemu if not building for host.
# if building for host, we already import android-emu.
$(call emugl-import,libandroidemu)

LOCAL_HEADER_LIBRARIES += \
    vulkan_headers \

endif

LOCAL_SRC_FILES := \
	VkEncoder.cpp \
	VulkanStream.cpp \

LOCAL_CFLAGS += -DLOG_TAG=\"emuglvk_enc\"

$(call emugl-export,C_INCLUDES,$(LOCAL_PATH))
$(call emugl-import,libOpenglCodecCommon$(GOLDFISH_OPENGL_LIB_SUFFIX))

$(call emugl-end-module)


