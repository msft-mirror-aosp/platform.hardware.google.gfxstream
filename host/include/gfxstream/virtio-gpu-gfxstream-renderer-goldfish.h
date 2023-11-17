// Copyright 2023 The Android Open Source Project
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

#include <cstdint>

#include "gfxstream/virtio-gpu-gfxstream-renderer.h"
#include "host-common/goldfish_pipe.h"

#ifdef __cplusplus
extern "C" {
#endif

VG_EXPORT void stream_renderer_set_service_ops(const GoldfishPipeServiceOps* ops);

VG_EXPORT int stream_renderer_snapshot_presave_pause();
VG_EXPORT int stream_renderer_snapshot_save(void* stream, void* textureSaver);
VG_EXPORT int stream_renderer_snapshot_postsave_resume();
VG_EXPORT int stream_renderer_snapshot_postsave_resume_for_testing();
VG_EXPORT int stream_renderer_snapshot_load(void* stream, void* textureSaver);

#ifdef __cplusplus
}  // extern "C"
#endif
