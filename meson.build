# Copyright 2023 Android Open Source Project
# SPDX-License-Identifier: MIT

project('gfxstream_backend', 'cpp', 'c',
         version : '0.0.1',
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
  '-Wno-reorder-ctor',
  '-Wno-mismatched-tags',
  '-Wno-missing-field-initializers',
  '-Wno-implicit-fallthrough',
  '-Wno-unused-private-field',
  '-Wno-macro-redefined'
]

#===============#
# Dependencies  #
#===============#
drm_dep = dependency('libdrm')
aemu_base_dep = dependency('aemu_base')
aemu_common_dep = dependency('aemu_host_common')
logging_base_dep = dependency('logging_base')

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

#===============#
# Includes      #
#===============#

gfxstream_header = files('include/render-utils/virtio-gpu-gfxstream-renderer.h')

inc_root = include_directories('.')
inc_include = include_directories('include')
inc_utils = include_directories('utils/include')

if use_vulkan
  inc_renderdoc_external = include_directories('third-party/renderdoc/include')
endif

if use_magma
  inc_magma_external = include_directories('third-party/fuchsia/magma/include')
  inc_magma_external_lib = include_directories('third-party/fuchsia/magma/include/lib')
endif

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
         'buildtype': (' ').join(get_option('buildtype')),
         'gles': use_gles,
         'vulkan': use_vulkan,
         'magma': use_magma,
         'composer': use_composer,
        }, section: 'Configuration')
