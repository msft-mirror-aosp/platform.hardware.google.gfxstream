// Copyright (C) 2018 The Android Open Source Project
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

// Protect from non-C++11 builds
#ifdef GOLDFISH_VULKAN
#pragma once

#include "IOStream.h"

class VkEncoder {
public:
    VkEncoder(IOStream*) { }
    ~VkEncoder() { }
};

#else

#ifndef VK_ENCODER_H
#define VK_ENCODER_H

class IOStream;

// Placeholder version to make non-C++11 happy
class VkEncoder {
public:
    VkEncoder(IOStream*) { }
    ~VkEncoder() { }
};

#endif // VK_ENCODER_H


#endif