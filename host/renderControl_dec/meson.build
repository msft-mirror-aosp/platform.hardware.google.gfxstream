# Copyright 2023 Android Open Source Project
# SPDX-License-Identifier: MIT

files_lib_composer = files(
  'renderControl_dec.cpp',
  'renderControl_server_context.cpp'
)

lib_composer = static_library(
  'composer',
  files_lib_composer,
  cpp_args: default_cpp_args,
  include_directories: [inc_include, inc_apigen_codec],
)
