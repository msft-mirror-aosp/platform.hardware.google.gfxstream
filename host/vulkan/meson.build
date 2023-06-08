# Copyright 2023 Android Open Source Project
# SPDX-License-Identifier: MIT

inc_vulkan_cereal = include_directories('cereal')
inc_vulkan_server = include_directories('.')

subdir('cereal')
subdir('emulated_textures')

files_lib_vulkan_server = files(
  'BorrowedImageVk.cpp',
  'BufferVk.cpp',
  'ColorBufferVk.cpp',
  'CompositorVk.cpp',
  'DisplayVk.cpp',
  'DisplaySurfaceVk.cpp',
  'PostWorkerVk.cpp',
  'DebugUtilsHelper.cpp',
  'SwapChainStateVk.cpp',
  'RenderThreadInfoVk.cpp',
  'VkAndroidNativeBuffer.cpp',
  'VkCommonOperations.cpp',
  'VkDecoder.cpp',
  'VkDecoderGlobalState.cpp',
  'VkDecoderSnapshot.cpp',
  'VkFormatUtils.cpp',
  'VkReconstruction.cpp',
  'VulkanDispatch.cpp',
  'VulkanHandleMapping.cpp',
  'VulkanStream.cpp',
  'vk_util.cpp'
)

vulkan_server_cpp_args = [
  '-fvisibility=hidden',
  '-Wno-inconsistent-missing-override',
  '-Wno-unused-value',
  '-Wno-return-type',
  '-Wno-return-type-c-linkage',
  '-Wno-unused-parameter',
  '-Wno-unused-variable',
  '-Wno-unused-function',
  '-Wno-uninitialized',
  '-DVK_GFXSTREAM_STRUCTURE_TYPE_EXT',
]

# Shouldn't these be defined by standard Vulkan headers?
if host_machine.system() == 'darwin'
  vulkan_server_cpp_args += '-DVK_USE_PLATFORM_METAL_EXT'
elif host_machine.system() == 'windows'
  vulkan_server_cpp_args += '-DVK_USE_PLATFORM_WIN32_KHR'
endif

lib_vulkan_server = static_library(
  'lib_vulkan_server',
  files_lib_vulkan_server,
  cpp_args:vulkan_server_cpp_args,
  include_directories: [inc_root, inc_include, inc_utils, inc_apigen_codec,
                        inc_vulkan_cereal, inc_stream_servers,
                        inc_vulkan_server, inc_renderdoc_external,
                        inc_gl_host_common],
  link_with: [lib_vulkan_cereal, lib_emulated_textures]
)
