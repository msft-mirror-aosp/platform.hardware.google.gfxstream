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

#===============#
# Dependencies  #
#===============#
aemu_base_dep = dependency('aemu_base')
aemu_common_dep = dependency('aemu_host_common')
logging_base_dep = dependency('logging_base')

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
  'include/render-utils/virtio-gpu-gfxstream-renderer.h',
  'include/render-utils/virtio-gpu-gfxstream-renderer-unstable.h')

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
