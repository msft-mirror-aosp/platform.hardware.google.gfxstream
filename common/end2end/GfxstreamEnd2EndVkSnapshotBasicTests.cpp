// Copyright (C) 2023 The Android Open Source Project
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

#include <string>

#include "GfxstreamEnd2EndTests.h"
#include "aemu/base/files/StdioStream.h"
#include "gfxstream/virtio-gpu-gfxstream-renderer-goldfish.h"
#include "host-common/feature_control.h"
#include "snapshot/TextureLoader.h"
#include "snapshot/TextureSaver.h"
#include "snapshot/common.h"

namespace gfxstream {
namespace tests {
namespace {

class GfxstreamEnd2EndVkSnapshotBasicTest : public GfxstreamEnd2EndTest {};

TEST_P(GfxstreamEnd2EndVkSnapshotBasicTest, BasicSaveLoad) {
    auto [instance, physicalDevice, device, queue, queueFamilyIndex] =
        VK_ASSERT(SetUpTypicalVkTestEnvironment());
    std::string snapshotFileName = testing::TempDir() + "snapshot.bin";
    std::string textureFileName = testing::TempDir() + "texture.bin";
    std::unique_ptr<android::base::StdioStream> stream(new android::base::StdioStream(
        fopen(snapshotFileName.c_str(), "wb"), android::base::StdioStream::kOwner));
    android::snapshot::ITextureSaverPtr textureSaver(
        new android::snapshot::TextureSaver(android::base::StdioStream(
            fopen(textureFileName.c_str(), "wb"), android::base::StdioStream::kOwner)));
    stream_renderer_snapshot_presave_pause();
    stream_renderer_snapshot_save(stream.get(), &textureSaver);
    stream.reset();
    textureSaver.reset();
    stream.reset(new android::base::StdioStream(fopen(snapshotFileName.c_str(), "rb"),
                                                android::base::StdioStream::kOwner));
    android::snapshot::ITextureLoaderPtr textureLoader(
        new android::snapshot::TextureLoader(android::base::StdioStream(
            fopen(textureFileName.c_str(), "rb"), android::base::StdioStream::kOwner)));
    stream_renderer_snapshot_load(stream.get(), &textureLoader);
    stream_renderer_snapshot_postsave_resume_for_testing();
}

INSTANTIATE_TEST_CASE_P(GfxstreamEnd2EndTests, GfxstreamEnd2EndVkSnapshotBasicTest,
                        ::testing::ValuesIn({
                            TestParams{
                                .with_gl = false,
                                .with_vk = true,
                                .with_vk_snapshot = true,
                            },
                        }),
                        &GetTestName);

}  // namespace
}  // namespace tests
}  // namespace gfxstream