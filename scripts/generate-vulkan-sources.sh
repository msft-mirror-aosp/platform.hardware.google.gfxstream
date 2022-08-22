#!/bin/sh

# Copyright 2022 The Android Open Source Project
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

PROJECT_ROOT=$(pwd)

# Generate Vulkan headers
cd registry/vulkan/xml && make
if [ $? -ne 0 ]; then
    echo "Failed to generate Vulkan headers." 1>&2
    exit $?
fi
rm -rf $PROJECT_ROOT/include/vulkan && mkdir -p $PROJECT_ROOT/include
if [ $? -ne 0 ]; then
    echo "Failed to clear the old Vulkan headers." 1>&2
    exit $?
fi
mv ../gen $PROJECT_ROOT/include/vulkan
if [ $? -ne 0 ]; then
    echo "Failed to move the new Vulkan headers to the target folder." 1>&2
    exit $?
fi

cd $PROJECT_ROOT

AOSP_DIR=$(pwd)/../../
export VK_CEREAL_GUEST_ENCODER_DIR=$AOSP_DIR/device/generic/goldfish-opengl/system/vulkan_enc
export VK_CEREAL_GUEST_HAL_DIR=$AOSP_DIR/device/generic/goldfish-opengl/system/vulkan
export VK_CEREAL_HOST_DECODER_DIR=$AOSP_DIR/device/generic/vulkan-cereal/stream-servers/vulkan
export VK_CEREAL_HOST_INCLUDE_DIR=$AOSP_DIR/device/generic/vulkan-cereal/stream-servers
export VK_CEREAL_BASELIB_PREFIX=base
export VK_CEREAL_BASELIB_LINKNAME=gfxstream-base.headers
export VK_CEREAL_VK_HEADER_TARGET=gfxstream_vulkan_headers

VK_CEREAL_OUTPUT_DIR=$VK_CEREAL_HOST_DECODER_DIR/cereal
mkdir -p $VK_CEREAL_GUEST_HAL_DIR
mkdir -p $VK_CEREAL_GUEST_HAL_DIR
mkdir -p $VK_CEREAL_OUTPUT_DIR

VULKAN_REGISTRY_DIR=$AOSP_DIR/external/gfxstream-protocols/registry/vulkan
VULKAN_REGISTRY_XML_DIR=$VULKAN_REGISTRY_DIR/xml
VULKAN_REGISTRY_SCRIPTS_DIR=$VULKAN_REGISTRY_DIR/scripts

python3 $VULKAN_REGISTRY_SCRIPTS_DIR/genvk.py -registry $VULKAN_REGISTRY_XML_DIR/vk.xml cereal -o $VK_CEREAL_OUTPUT_DIR
