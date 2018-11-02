LOCAL_PATH := $(call my-dir)

$(call emugl-begin-static-library,libandroidemu)
$(call emugl-export,C_INCLUDES,$(LOCAL_PATH))

LOCAL_CFLAGS += \
    -DLOG_TAG=\"androidemu\" \
    -Wno-missing-field-initializers \
    -fvisibility=default \
    -fstrict-aliasing \

LOCAL_SRC_FILES := \
    android/base/files/MemStream.cpp \
    android/base/files/Stream.cpp \
    android/base/files/StreamSerializing.cpp \
    android/base/Log.cpp \
    android/base/Pool.cpp \
    android/base/StringFormat.cpp \
    android/utils/debug.c \
    android/utils/debug_wrapper.cpp \

$(call emugl-end-module)
