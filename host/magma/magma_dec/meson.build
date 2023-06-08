# Copyright 2023 Android Open Source Project
# SPDX-License-Identifier: MIT

inc_magma_dec = include_directories('.')

files_lib_magma_dec = files(
  'magma_dec.cpp',
  'magma_server_context.cpp',
)

lib_magma_dec = static_library(
  'magma_dec',
  files_lib_magma_dec,
  cpp_args: default_cpp_args,
  include_directories: [inc_include, inc_magma_external, inc_apigen_codec],
)
