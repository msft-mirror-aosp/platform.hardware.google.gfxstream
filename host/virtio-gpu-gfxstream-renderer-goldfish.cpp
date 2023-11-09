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

#include "gfxstream/virtio-gpu-gfxstream-renderer-goldfish.h"

#include "host-common/opengles.h"

VG_EXPORT int stream_renderer_snapshot_presave_pause() {
    android_getOpenglesRenderer()->pauseAllPreSave();
    return 0;
}

VG_EXPORT int stream_renderer_snapshot_postsave_resume() {
    android_getOpenglesRenderer()->resumeAll();
    return 0;
}

// In end2end tests, we don't really do snapshot save for render threads.
// We will need to resume all render threads without waiting for snapshot.
VG_EXPORT int stream_renderer_snapshot_postsave_resume_for_testing() {
    android_getOpenglesRenderer()->resumeAll(false);
    return 0;
}

VG_EXPORT int stream_renderer_snapshot_save(void* stream, void* textureSaver) {
    android_getOpenglesRenderer()->save(
        static_cast<android::base::Stream*>(stream),
        *static_cast<const android::snapshot::ITextureSaverPtr*>(textureSaver));
    return 0;
}

VG_EXPORT int stream_renderer_snapshot_load(void* stream, void* textureLoader) {
    android_getOpenglesRenderer()->load(
        static_cast<android::base::Stream*>(stream),
        *static_cast<const android::snapshot::ITextureLoaderPtr*>(textureLoader));
    return 0;
}
