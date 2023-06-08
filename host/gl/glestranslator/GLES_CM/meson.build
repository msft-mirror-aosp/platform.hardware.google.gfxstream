# Copyright 2023 Android Open Source Project
# SPDX-License-Identifier: MIT

files_lib_glescm_translator = files(
  'CoreProfileEngine.cpp',
  'GLEScmContext.cpp',
  'GLEScmImp.cpp',
  'GLEScmUtils.cpp',
  'GLEScmValidate.cpp',
)

lib_glescm_translator = static_library(
  'glescm_translator',
  files_lib_glescm_translator,
  cpp_args: default_cpp_args,
  include_directories: [inc_include, inc_gles_translator],
)
