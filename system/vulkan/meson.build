# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

files_lib_vulkan_cereal = files(
  'goldfish_vulkan.cpp',
)

lib_vulkan_cereal = shared_library(
   'vulkan_cereal',
   files_lib_vulkan_cereal,
   cpp_args: cpp_args,
   include_directories: [inc_android_emu, inc_android_compat, inc_opengl_system,
			 inc_host, inc_opengl_codec, inc_render_enc,
                         inc_vulkan_enc],
   link_with: [lib_android_compat, lib_emu_android_base, lib_stream,
               lib_vulkan_enc]
)
