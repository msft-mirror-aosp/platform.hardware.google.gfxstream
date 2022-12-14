LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := CommandBufferStagingStreamTests

$(call emugl-import,libvulkan_enc)

LOCAL_C_INCLUDES += \
    device/generic/goldfish-opengl/host/include/libOpenglRender \


LOCAL_SRC_FILES:= \
    CommandBufferStagingStream_test.cpp \

LOCAL_STATIC_LIBRARIES := libgmock
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_TAGS := tests

include $(BUILD_NATIVE_TEST)
