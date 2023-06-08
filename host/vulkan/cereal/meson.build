# Copyright 2023 Android Open Source Project
# SPDX-License-Identifier: MIT

files_lib_cereal = files(
  'common/goldfish_vk_extension_structs.cpp',
  'common/goldfish_vk_marshaling.cpp',
  'common/goldfish_vk_reserved_marshaling.cpp',
  'common/goldfish_vk_testing.cpp',
  'common/goldfish_vk_deepcopy.cpp',
  'common/goldfish_vk_handlemap.cpp',
  'common/goldfish_vk_dispatch.cpp',
  'common/goldfish_vk_transform.cpp'
)

cereal_cpp_args = [
  '-DVK_GFXSTREAM_STRUCTURE_TYPE_EXT',
  '-DVK_GOOGLE_address_space',
]

lib_vulkan_cereal = static_library(
  'cereal',
  files_lib_cereal,
  cpp_args: cereal_cpp_args,
  include_directories: [inc_stream_servers, inc_vulkan_server, inc_utils,
                        inc_include],
)
