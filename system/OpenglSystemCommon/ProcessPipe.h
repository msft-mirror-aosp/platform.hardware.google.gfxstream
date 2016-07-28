//
// Copyright (C) 2016 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once

#include <stdint.h>

// The process pipe is used to notify the host about process exits,
// also append a process unique ID (puid) to all encoder calls which create or
// release GL resources owned by process. This is for the purpose that the host
// can clean up process resources when a process is killed. It will fallback
// to the default path if the host does not support it. Processes are identified
// by acquiring a per-process 64bit unique ID (puid) from the host.
//
// Note: you don't need call this function directly. Use it through PUID_CMD.

bool processPipeInit();

// Return per-process unique ID. This ID is assigned by the host. It is
// initialized when calling processPipeInit().
//
// Note: you don't need to use this function directly
uint64_t getProcUID();

// Associate PUID (process unique ID) with resource create / release commands
// See the comments in processPipeInit() for more details
//
// Example:
// uint32_t img = PUID_CMD(rcEnc, rcCreateClientImage, ctxHandle, target, texture);
//
#define PUID_CMD(encoder, func, ...) \
    (processPipeInit() ? (encoder)->func##Puid((encoder), __VA_ARGS__, getProcUID()) \
                       : (encoder)->func((encoder), __VA_ARGS__))
