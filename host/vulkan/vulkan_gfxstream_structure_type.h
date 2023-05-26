// Copyright (C) 2018 The Android Open Source Project
// Copyright (C) 2018 Google Inc.
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

// Autogenerated module vulkan_gfxstream_structure_type
//
// (header) generated by registry/vulkan/scripts/genvk.py -registry registry/vulkan/xml/vk.xml
// cereal -o ../../device/generic/vulkan-cereal/host/vulkan/cereal
//
// Please do not modify directly;
// re-run gfxstream-protocols/scripts/generate-vulkan-sources.sh,
// or directly from Python by defining:
// VULKAN_REGISTRY_XML_DIR : Directory containing vk.xml
// VULKAN_REGISTRY_SCRIPTS_DIR : Directory containing genvk.py
// CEREAL_OUTPUT_DIR: Where to put the generated sources.
//
// python3 $VULKAN_REGISTRY_SCRIPTS_DIR/genvk.py -registry $VULKAN_REGISTRY_XML_DIR/vk.xml cereal -o
// $CEREAL_OUTPUT_DIR
//
#pragma once

#define VK_GOOGLE_GFXSTREAM_ENUM(type, id) \
    ((type)(1000000000 + (1000 * (VK_GOOGLE_GFXSTREAM_NUMBER - 1)) + (id)))

#define VK_STRUCTURE_TYPE_IMPORT_COLOR_BUFFER_GOOGLE VK_GOOGLE_GFXSTREAM_ENUM(VkStructureType, 0)
#define VK_STRUCTURE_TYPE_IMPORT_BUFFER_GOOGLE VK_GOOGLE_GFXSTREAM_ENUM(VkStructureType, 1)
#define VK_STRUCTURE_TYPE_CREATE_BLOB_GOOGLE VK_GOOGLE_GFXSTREAM_ENUM(VkStructureType, 2)
