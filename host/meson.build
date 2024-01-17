# Copyright 2023 Android Open Source Project
# SPDX-License-Identifier: MIT

inc_stream_servers = include_directories('.')
inc_host_include = include_directories('include')

subdir('apigen-codec-common')
subdir('gl/gl-host-common')

inc_gfxstream_backend = [inc_root, inc_include, inc_apigen_codec, inc_utils,
                         inc_gl_host_common, inc_host_include]

link_gfxstream_backend = [lib_gl_host_common]

files_lib_gfxstream_backend = files(
  'Buffer.cpp',
  'BlobManager.cpp',
  'ChannelStream.cpp',
  'ColorBuffer.cpp',
  'DisplaySurface.cpp',
  'DisplaySurfaceUser.cpp',
  'Hwc2.cpp',
  'PostWorker.cpp',
  'ReadBuffer.cpp',
  'render_api.cpp',
  'RenderChannelImpl.cpp',
  'RenderThread.cpp',
  'RenderThreadInfo.cpp',
  'RingStream.cpp',
  'SyncThread.cpp',
  'RenderControl.cpp',
  'RenderWindow.cpp',
  'RenderLibImpl.cpp',
  'RendererImpl.cpp',
  'FrameBuffer.cpp',
  'GfxStreamAgents.cpp',
  'virtio-gpu-gfxstream-renderer.cpp',
  'VirtioGpuTimelines.cpp',
  'VsyncThread.cpp',
)

if use_gles or use_vulkan
  subdir('compressedTextureFormats')
endif

if use_gles
  subdir('gl')

  files_lib_gfxstream_backend += files('PostWorkerGl.cpp')
  files_lib_gfxstream_backend += files('RenderThreadInfoGl.cpp')

  inc_gfxstream_backend += [inc_gl_server, inc_gl_snapshot, inc_gles_translator]
  link_gfxstream_backend += lib_gl_server
endif

if use_vulkan
  subdir('vulkan')
  inc_gfxstream_backend += [inc_cereal, inc_cereal_common, inc_vulkan_headers,
                            inc_vulkan_server, inc_renderdoc_external]
  link_gfxstream_backend += lib_vulkan_server
endif

if use_composer
  subdir('renderControl_dec')
  link_gfxstream_backend += lib_composer
endif

if use_magma
  subdir('magma')
  files_lib_gfxstream_backend += files('RenderThreadInfoMagma.cpp')
  inc_gfxstream_backend += [inc_magma_dec, inc_magma_external]
  link_gfxstream_backend += lib_magma_server
endif

if host_machine.system() == 'darwin'
  files_lib_gfxstream_backend += files('NativeSubWindow_cocoa.m')
elif host_machine.system() == 'windows'
  files_lib_gfxstream_backend += files('NativeSubWindow_win32.cpp')
elif host_machine.system() == 'linux'
  files_lib_gfxstream_backend += files('NativeSubWindow_x11.cpp')
elif host_machine.system() == 'qnx'
  files_lib_gfxstream_backend += files(
    'NativeSubWindow_qnx.cpp',
    '../qnx/host/platform_qnx.cpp',
  )
endif

gfxstream_backend_cpp_args = [
  '-Wno-unused-parameter',
  '-Wno-unused-variable',
  '-Wno-unused-function',
  '-DVK_GFXSTREAM_STRUCTURE_TYPE_EXT',
]

deps_gfxstream_backend = [
  aemu_common_dep,
  aemu_base_dep,
  aemu_logging_dep,
  aemu_snapshot_dep,
]

link_args_gfxstream_backend = ''

if host_machine.system() == 'linux'
  deps_gfxstream_backend += [
    dl_dep,
    thread_dep,
  ]
  link_args_gfxstream_backend = '-Wl,-lpthread,-lrt'
endif

if host_machine.system() == 'qnx'
  deps_gfxstream_backend += [
    qnx_egl_dep,
    qnx_gles2_dep,
    qnx_screen_dep,
  ]
endif

gfxstream_backend = library(
  'gfxstream_backend',
  files_lib_gfxstream_backend,
  cpp_args: gfxstream_host_args + gfxstream_backend_cpp_args,
  include_directories: [inc_gfxstream_backend, inc_glm],
  gnu_symbol_visibility: 'default',
  dependencies: deps_gfxstream_backend,
  link_with: link_gfxstream_backend,
  link_args : link_args_gfxstream_backend,
  version: '0.1.2',
  install: true,
)

install_headers(gfxstream_headers,
                subdir: 'gfxstream')

pkg = import('pkgconfig')
pkg.generate(gfxstream_backend,
             description: 'gfxstream backend',
             subdirs: 'gfxstream'
)
