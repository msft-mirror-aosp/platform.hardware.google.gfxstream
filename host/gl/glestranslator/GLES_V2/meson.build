# Copyright 2023 Android Open Source Project
# SPDX-License-Identifier: MIT

files_lib_glesv2_translator = files(
  'ANGLEShaderParser.cpp',
  'GLESv2Context.cpp',
  'GLESv2Imp.cpp',
  'GLESv2Validate.cpp',
  'ProgramData.cpp',
  'SamplerData.cpp',
  'ShaderParser.cpp',
  'ShaderValidator.cpp',
  'TransformFeedbackData.cpp',
)

lib_glesv2_translator = static_library(
  'glesv2_translator',
  files_lib_glesv2_translator,
  cpp_args: default_cpp_args,
  include_directories: [inc_include, inc_gles_translator, inc_stream_servers]
)
