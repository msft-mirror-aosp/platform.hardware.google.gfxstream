/*
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GOLDFISH_MEDIA_H264_DEC_H_
#define GOLDFISH_MEDIA_H264_DEC_H_

struct h264_result_t {
    int ret;
    uint64_t bytesProcessed;
};

struct h264_image_t {
    const uint8_t* data;
    uint32_t width;
    uint32_t height;
    // on success, |ret| will indicate the size of |data|.
    // If failed, |ret| will contain some negative error code.
    int ret;
};

class MediaH264Decoder {
public:
    MediaH264Decoder() = default;
    virtual ~MediaH264Decoder() = default;

    enum class PixelFormat : uint8_t {
        YUV420P = 0,
        UYVY422 = 1,
        BGRA8888 = 2,
    };

    enum class Err : int {
        NoErr = 0,
        NoDecodedFrame = -1,
        InitContextFailed = -2,
        DecoderRestarted = -3,
        NALUIgnored = -4,
    };

    void initH264Context(unsigned int width,
                         unsigned int height,
                         unsigned int outWidth,
                         unsigned int outHeight,
                         PixelFormat pixFmt);
    void destroyH264Context();
    h264_result_t decodeFrame(uint8_t* img, size_t szBytes);
    void flush();
    h264_image_t getImage();
};
#endif
