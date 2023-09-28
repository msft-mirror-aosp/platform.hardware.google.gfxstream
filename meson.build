# Copyright 2023 Android Open Source Project
# SPDX-License-Identifier: MIT

project('gfxstream_backend', 'cpp', 'c',
         version : '0.1.2',
         license : 'MIT OR Apache-2.0',
         default_options : ['cpp_std=gnu++17'])

cc = meson.get_compiler('cpp')
prog_python = import('python').find_installation('python3')

#===============#
# Configuration #
#===============#
c_args = []
cpp_args = []

default_cpp_args = [
  '-D_FILE_OFFSET_BITS=64',
  '-Wno-unused-parameter',
  '-Wno-unused-function',
  '-Wno-unused-variable',
  '-Wno-ignored-qualifiers',
  '-Wno-mismatched-tags',
  '-Wno-missing-field-initializers',
  '-Wno-implicit-fallthrough',
]

if host_machine.system() == 'qnx'
  default_cpp_args += '-D_QNX_SOURCE'
  qnx_target = get_option('qnx_target')
  if qnx_target == ''
    error('option qnx_target is not set')
  endif
endif

#===============#
# Dependencies  #
#===============#
if host_machine.system() == 'qnx'
  ## have not yet got pkgconfig to work with cross-compile,
  ## finding libraries manually in the meantime.

  ## ERROR: Dependency "screen" not found, tried pkgconfig
  # qnx_screen_dep = dependency('screen')

  rel_path_prefix = meson.get_external_property('qnx_path_prefix')
  abs_path_prefix = meson.current_source_dir() + '/' + rel_path_prefix

  aemu_libs_path = abs_path_prefix + '/aemu/install/lib'

  incl_aemu_headers = include_directories([
    rel_path_prefix + '/aemu/install/include',
    rel_path_prefix + '/aemu/install/include/aemu/host-common',
    rel_path_prefix + '/aemu/install/include/aemu/snapshot',
  ])

  aemu_base_lib = cc.find_library('aemu-base', dirs: aemu_libs_path)
  aemu_base_dep = declare_dependency(include_directories : incl_aemu_headers, dependencies : [aemu_base_lib])

  aemu_common_lib = cc.find_library('aemu-host-common', dirs: aemu_libs_path)
  aemu_common_dep = declare_dependency(include_directories : incl_aemu_headers, dependencies : [aemu_common_lib])

  aemu_logging_lib = cc.find_library('aemu-logging', dirs: aemu_libs_path)
  aemu_logging_dep = declare_dependency(include_directories : incl_aemu_headers, dependencies : [aemu_logging_lib])

  aemu_snapshot_lib = cc.find_library('aemu-snapshot', dirs: aemu_libs_path)
  aemu_snapshot_dep = declare_dependency(include_directories : incl_aemu_headers, dependencies : [aemu_snapshot_lib])

  inc_qnx_headers = include_directories(join_paths(qnx_target, 'usr/include'))
  qnx_screen_lib = cc.find_library('screen', required : true)
  qnx_screen_dep = declare_dependency(include_directories: inc_qnx_headers, dependencies: [qnx_screen_lib])

else
  aemu_base_dep = dependency('aemu_base')
  aemu_common_dep = dependency('aemu_host_common')
  aemu_logging_dep = dependency('aemu_logging')
  aemu_snapshot_dep = dependency('aemu_snapshot')
endif

#========================#
# Logging + error report #
#========================#
log_level = get_option('log-level')

if log_level == 'error'
  default_cpp_args += '-DSTREAM_RENDERER_LOG_LEVEL=1'
elif log_level == 'warn'
  default_cpp_args += '-DSTREAM_RENDERER_LOG_LEVEL=2'
elif log_level == 'info'
  default_cpp_args += '-DSTREAM_RENDERER_LOG_LEVEL=3'
endif

#===============#
# Decoders      #
#===============#
decoders = get_option('decoders')

use_auto = decoders.contains('auto')
use_gles = decoders.contains('gles')
use_vulkan = decoders.contains('vulkan')
use_magma = decoders.contains('magma')
use_composer = decoders.contains('composer')

if use_auto and (use_gles or use_vulkan or use_magma)
  error('Can not specify auto and custom options are same time')
endif

if use_auto
  use_gles = true
  use_vulkan = true
  use_composer = true
  use_magma = host_machine.system() == 'linux'
endif

if use_magma
  default_cpp_args += '-DUSE_MAGMA=1'
  drm_dep = dependency('libdrm')
else
  default_cpp_args += '-DUSE_MAGMA=0'
endif

#===============#
# Includes      #
#===============#

gfxstream_headers = files(
  'host/include/gfxstream/virtio-gpu-gfxstream-renderer.h',
  'host/include/gfxstream/virtio-gpu-gfxstream-renderer-unstable.h')

inc_root = include_directories('.')
inc_include = include_directories('include')
inc_utils = include_directories('utils/include')

if use_vulkan
  inc_vulkan_headers = include_directories('common/vulkan/include')
  inc_renderdoc_external = include_directories('third-party/renderdoc/include')
endif

if use_magma
  inc_magma_external = include_directories('third-party/fuchsia/magma/include')
  inc_magma_external_lib = include_directories('third-party/fuchsia/magma/include/lib')
endif

inc_glm = include_directories('third-party/glm/include')

#================#
# Subdirectories #
#================#

subdir('gl-host-common')
subdir('host')

#================#
# Summary        #
#================#

summary({'prefix': get_option('prefix'),
         'libdir': get_option('libdir'),
        }, section: 'Directories')
summary({'c_args': (' ').join(get_option('c_args')),
         'cpp_args': (' ').join(get_option('cpp_args')),
         'buildtype': get_option('buildtype'),
         'log-level': log_level,
         'gles': use_gles,
         'vulkan': use_vulkan,
         'magma': use_magma,
         'composer': use_composer,
        }, section: 'Configuration')
