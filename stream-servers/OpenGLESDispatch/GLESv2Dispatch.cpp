/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include "OpenGLESDispatch/GLESv2Dispatch.h"

#include "OpenGLESDispatch/EGLDispatch.h"
#include "OpenGLESDispatch/StaticDispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_GLES_V2_LIB EMUGL_LIBNAME("GLES_V2_translator")

// An unimplemented function which prints out an error message.
// To make it consistent with the guest, all GLES2 functions not supported by
// the driver should be redirected to this function.

void gles2_unimplemented() {
    fprintf(stderr, "Called unimplemented GLES API\n");
}

#define LOOKUP_SYMBOL_STATIC(return_type,function_name,signature,callargs) \
    dispatch_table-> function_name = reinterpret_cast< function_name ## _t >( \
            translator::gles2::function_name); \
    if ((!dispatch_table-> function_name) && s_egl.eglGetProcAddress) \
        dispatch_table-> function_name = reinterpret_cast< function_name ## _t >( \
            s_egl.eglGetProcAddress(#function_name)); \

bool gles2_dispatch_init(GLESv2Dispatch* dispatch_table) {
    if (dispatch_table->initialized) return true;

    LIST_GLES2_FUNCTIONS(LOOKUP_SYMBOL_STATIC,LOOKUP_SYMBOL_STATIC)
   
    dispatch_table->initialized = true;
    return true;
}
