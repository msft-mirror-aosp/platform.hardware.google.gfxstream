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

// Autogenerated module func_table
//
// (header) generated by codegen/vulkan/vulkan-docs/scripts/genvk.py -registry
// codegen/vulkan/vulkan-docs/xml/vk.xml -registryGfxstream
// codegen/vulkan/vulkan-docs/xml/vk_gfxstream.xml cereal -o host/vulkan/cereal
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
#include <vulkan/vulkan.h>

#include "vulkan_gfxstream.h"

namespace gfxstream {
namespace vk {

#ifdef VK_VERSION_1_0
#endif
#ifdef VK_VERSION_1_1
#endif
#ifdef VK_VERSION_1_2
#endif
#ifdef VK_VERSION_1_3
#endif
#ifdef VK_KHR_surface
#endif
#ifdef VK_KHR_swapchain
#endif
#ifdef VK_KHR_xcb_surface
#endif
#ifdef VK_KHR_android_surface
#endif
#ifdef VK_KHR_win32_surface
#endif
#ifdef VK_KHR_dynamic_rendering
#endif
#ifdef VK_KHR_get_physical_device_properties2
#endif
#ifdef VK_KHR_maintenance1
#endif
#ifdef VK_KHR_external_memory_capabilities
#endif
#ifdef VK_KHR_external_memory
#endif
#ifdef VK_KHR_external_memory_win32
#endif
#ifdef VK_KHR_external_memory_fd
#endif
#ifdef VK_KHR_external_semaphore_capabilities
#endif
#ifdef VK_KHR_external_semaphore
#endif
#ifdef VK_KHR_external_semaphore_win32
#endif
#ifdef VK_KHR_external_semaphore_fd
#endif
#ifdef VK_KHR_shader_float16_int8
#endif
#ifdef VK_KHR_incremental_present
#endif
#ifdef VK_KHR_descriptor_update_template
#endif
#ifdef VK_KHR_imageless_framebuffer
#endif
#ifdef VK_KHR_create_renderpass2
#endif
#ifdef VK_KHR_external_fence_capabilities
#endif
#ifdef VK_KHR_external_fence
#endif
#ifdef VK_KHR_external_fence_fd
#endif
#ifdef VK_KHR_maintenance2
#endif
#ifdef VK_KHR_dedicated_allocation
#endif
#ifdef VK_KHR_storage_buffer_storage_class
#endif
#ifdef VK_KHR_get_memory_requirements2
#endif
#ifdef VK_KHR_image_format_list
#endif
#ifdef VK_KHR_sampler_ycbcr_conversion
#endif
#ifdef VK_KHR_bind_memory2
#endif
#ifdef VK_KHR_maintenance3
#endif
#ifdef VK_KHR_shader_subgroup_extended_types
#endif
#ifdef VK_KHR_vulkan_memory_model
#endif
#ifdef VK_KHR_shader_terminate_invocation
#endif
#ifdef VK_KHR_buffer_device_address
#endif
#ifdef VK_KHR_pipeline_executable_properties
#endif
#ifdef VK_KHR_shader_integer_dot_product
#endif
#ifdef VK_KHR_shader_non_semantic_info
#endif
#ifdef VK_KHR_synchronization2
#endif
#ifdef VK_KHR_zero_initialize_workgroup_memory
#endif
#ifdef VK_KHR_copy_commands2
#endif
#ifdef VK_KHR_format_feature_flags2
#endif
#ifdef VK_KHR_maintenance4
#endif
#ifdef VK_ANDROID_native_buffer
#endif
#ifdef VK_EXT_transform_feedback
#endif
#ifdef VK_AMD_gpu_shader_half_float
#endif
#ifdef VK_EXT_texture_compression_astc_hdr
#endif
#ifdef VK_EXT_swapchain_colorspace
#endif
#ifdef VK_EXT_queue_family_foreign
#endif
#ifdef VK_EXT_debug_utils
#endif
#ifdef VK_ANDROID_external_memory_android_hardware_buffer
#endif
#ifdef VK_EXT_inline_uniform_block
#endif
#ifdef VK_EXT_shader_stencil_export
#endif
#ifdef VK_EXT_pipeline_creation_feedback
#endif
#ifdef VK_NV_shader_subgroup_partitioned
#endif
#ifdef VK_EXT_metal_surface
#endif
#ifdef VK_EXT_subgroup_size_control
#endif
#ifdef VK_EXT_tooling_info
#endif
#ifdef VK_EXT_provoking_vertex
#endif
#ifdef VK_EXT_line_rasterization
#endif
#ifdef VK_EXT_index_type_uint8
#endif
#ifdef VK_EXT_extended_dynamic_state
#endif
#ifdef VK_EXT_swapchain_maintenance1
#endif
#ifdef VK_EXT_shader_demote_to_helper_invocation
#endif
#ifdef VK_EXT_texel_buffer_alignment
#endif
#ifdef VK_EXT_device_memory_report
#endif
#ifdef VK_EXT_custom_border_color
#endif
#ifdef VK_EXT_private_data
#endif
#ifdef VK_EXT_pipeline_creation_cache_control
#endif
#ifdef VK_EXT_ycbcr_2plane_444_formats
#endif
#ifdef VK_EXT_image_robustness
#endif
#ifdef VK_EXT_image_compression_control
#endif
#ifdef VK_EXT_4444_formats
#endif
#ifdef VK_EXT_primitive_topology_list_restart
#endif
#ifdef VK_EXT_extended_dynamic_state2
#endif
#ifdef VK_GOOGLE_gfxstream
#endif
#ifdef VK_EXT_load_store_op_none
#endif
#ifdef VK_EXT_image_compression_control_swapchain
#endif
void* goldfish_vulkan_get_proc_address(const char* name);
void* goldfish_vulkan_get_instance_proc_address(VkInstance instance, const char* name);
void* goldfish_vulkan_get_device_proc_address(VkDevice device, const char* name);

}  // namespace vk
}  // namespace gfxstream
