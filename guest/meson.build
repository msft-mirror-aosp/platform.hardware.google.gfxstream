# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

project('gfxstream_frontend', 'cpp', 'c',
         version : '0.1.2',
         license : 'MIT OR Apache-2.0',
         default_options : ['cpp_std=gnu++17'])

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
prog_python = import('python').find_installation('python3')

#===============#
# Options       #
#===============#

with_vulkan_icd_dir = get_option('vulkan-icd-dir')
if with_vulkan_icd_dir == ''
  with_vulkan_icd_dir = join_paths(get_option('datadir'), 'vulkan/icd.d')
endif

subdir('mesa')

#===============#
# Configuration #
#===============#
c_args = []
cpp_args = []

# Paravirtualization moving towards virtio
cpp_args += '-DVIRTIO_GPU'
# Gfxstream
cpp_args += '-DGFXSTREAM'
# Our internal guest build
cpp_args += '-DLINUX_GUEST_BUILD'
# Don't want to goldfish OpenGL
cpp_args += '-DGOLDFISH_NO_GL'
# This should just be called NO_TRACE eventually
cpp_args += '-DFUCHSIA_NO_TRACE'
# Include the gfxstream private VkStructureType definitions
cpp_args += '-DVK_GFXSTREAM_STRUCTURE_TYPE_EXT'

#===============#
# Dependencies  #
#===============#

dl_dep = cc.find_library('dl', required: false)
drm_dep = dependency('libdrm')
thread_dep = dependency('threads')

#===============#
# Includes      #
#===============#

inc_include_mesa = include_directories('mesa/include')
inc_vulkan_headers = include_directories('../common/vulkan/include')
inc_opengl_headers = include_directories('../common/opengl/include')
inc_fuchsia_magma = include_directories('../third-party/fuchsia/magma/include')
inc_fuchsia_magma_nolib = include_directories('../third-party/fuchsia/magma/include/lib')
inc_android_emu = include_directories('android-emu')
inc_gralloc = include_directories('gralloc_cb/include')
inc_magma = include_directories('magma')
inc_opengl_codec = include_directories('OpenglCodecCommon')
inc_opengl_system = include_directories('OpenglSystemCommon')
inc_render_enc = include_directories('renderControl_enc')
inc_system = include_directories('include')
inc_vulkan_enc = include_directories('vulkan_enc')

#================#
# Mesa compat    #
#================#
# dep_libdrm = drm_dep
# dep_dl = dl_dep
# dep_thread = thread_dep

#================#
# Subdirectories #
#================#

subdir('fuchsia')
subdir('iostream')
subdir('platform')
subdir('android-emu/aemu/base')
subdir('GoldfishAddressSpace')
subdir('qemupipe')
subdir('OpenglCodecCommon')
subdir('renderControl_enc')
subdir('OpenglSystemCommon')
subdir('magma')
subdir('vulkan_enc')
subdir('vulkan')
