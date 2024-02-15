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
#pragma once

// Interface between android-emu non-base libraries and emugl

#include "host-common/GoldfishDma.h"
#include "host-common/goldfish_sync.h"
#include "host-common/FeatureControl.h"

// Feature control
typedef bool (*emugl_feature_is_enabled_t)(android::featurecontrol::Feature feature);

// OpenGL timestamped logger
typedef void (*emugl_logger_t)(const char* fmt, ...);
typedef struct {
    emugl_logger_t coarse;
    emugl_logger_t fine;
} emugl_logger_struct;

