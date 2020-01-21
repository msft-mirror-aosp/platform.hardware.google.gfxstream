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

#include <utils/Log.h>

#include "MediaH264Decoder.h"
#include "goldfish_media_utils.h"
#include <string.h>

void MediaH264Decoder::initH264Context(unsigned int width,
                                       unsigned int height,
                                       unsigned int outWidth,
                                       unsigned int outHeight,
                                       PixelFormat pixFmt) {
    auto transport = GoldfishMediaTransport::getInstance();
    transport->writeParam(width, 0);
    transport->writeParam(height, 1);
    transport->writeParam(outWidth, 2);
    transport->writeParam(outHeight, 3);
    transport->writeParam(static_cast<uint64_t>(pixFmt), 4);
    transport->sendOperation(MediaCodecType::H264Codec,
                             MediaOperation::InitContext);
}

void MediaH264Decoder::destroyH264Context() {
    auto transport = GoldfishMediaTransport::getInstance();
    transport->sendOperation(MediaCodecType::H264Codec,
                             MediaOperation::DestroyContext);
}

h264_result_t MediaH264Decoder::decodeFrame(uint8_t* img, size_t szBytes, uint64_t pts) {
    auto transport = GoldfishMediaTransport::getInstance();
    uint8_t* hostSrc = transport->getInputAddr();
    if (img != nullptr && szBytes > 0) {
        memcpy(hostSrc, img, szBytes);
    }
    transport->writeParam(transport->offsetOf((uint64_t)(hostSrc)), 0);
    transport->writeParam((uint64_t)szBytes, 1);
    transport->writeParam((uint64_t)pts, 2);
    transport->sendOperation(MediaCodecType::H264Codec,
                             MediaOperation::DecodeImage);

    h264_result_t res = {0, 0};

    auto* retptr = transport->getReturnAddr();
    res.bytesProcessed = *(uint64_t*)(retptr);
    res.ret = *(int*)(retptr + 8);

    return res;
}

void MediaH264Decoder::flush() {
    auto transport = GoldfishMediaTransport::getInstance();
    transport->sendOperation(MediaCodecType::H264Codec,
                             MediaOperation::Flush);
}

h264_image_t MediaH264Decoder::getImage() {
    h264_image_t res { };
    auto transport = GoldfishMediaTransport::getInstance();
    uint8_t* dst = transport->getOutputAddr();
    transport->writeParam(transport->offsetOf((uint64_t)(dst)), 0);
    transport->sendOperation(MediaCodecType::H264Codec,
                             MediaOperation::GetImage);
    auto* retptr = transport->getReturnAddr();
    res.ret = *(int*)(retptr);
    if (res.ret >= 0) {
        res.data = dst;
        res.width = *(uint32_t*)(retptr + 8);
        res.height = *(uint32_t*)(retptr + 16);
        res.pts = *(uint32_t*)(retptr + 24);
        res.color_primaries = *(uint32_t*)(retptr + 32);
        res.color_range = *(uint32_t*)(retptr + 40);
        res.color_trc = *(uint32_t*)(retptr + 48);
        res.colorspace = *(uint32_t*)(retptr + 56);
    } else if (res.ret == (int)(Err::DecoderRestarted)) {
        res.width = *(uint32_t*)(retptr + 8);
        res.height = *(uint32_t*)(retptr + 16);
    }

    return res;
}
