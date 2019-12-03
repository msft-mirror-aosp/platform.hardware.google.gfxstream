#
# Copyright 2018 The Android Open-Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    GoldfishOMXComponent.cpp \
    GoldfishOMXPlugin.cpp \
    GoldfishVideoDecoderOMXComponent.cpp \
    SimpleGoldfishOMXComponent.cpp \


LOCAL_CFLAGS := $(PV_CFLAGS_MINUS_VISIBILITY) -Werror


LOCAL_C_INCLUDES:= \
        $(call include-path-for, frameworks-native)/media/hardware \
        $(call include-path-for, frameworks-native)/media/openmax \

LOCAL_HEADER_LIBRARIES := media_plugin_headers \
	                      libmedia_headers \
	                      libbinder_headers \
	                      libhidlbase_impl_internal \
	                      libbase

LOCAL_SHARED_LIBRARIES :=       \
        libbinder               \
        libutils                \
        liblog                  \
        libcutils               \
        android.hardware.media.omx@1.0 \
        libstagefright_foundation

LOCAL_MODULE := libstagefrighthw
LOCAL_VENDOR_MODULE := true

include $(BUILD_SHARED_LIBRARY)
