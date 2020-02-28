/*
 * Copyright (C) 2011 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#include <utils/Log.h>
#include <utils/misc.h>
//#include "OMX_VideoExt.h"

#include "GoldfishVPX.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>


namespace android {

// Only need to declare the highest supported profile and level here.
static const CodecProfileLevel kVP9ProfileLevels[] = {
    { OMX_VIDEO_VP9Profile0, OMX_VIDEO_VP9Level5 },
    { OMX_VIDEO_VP9Profile2, OMX_VIDEO_VP9Level5 },
    { OMX_VIDEO_VP9Profile2HDR, OMX_VIDEO_VP9Level5 },
    { OMX_VIDEO_VP9Profile2HDR10Plus, OMX_VIDEO_VP9Level5 },
};

GoldfishVPX::GoldfishVPX(
        const char *name,
        const char *componentRole,
        OMX_VIDEO_CODINGTYPE codingType,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : GoldfishVideoDecoderOMXComponent(
            name, componentRole, codingType,
            codingType == OMX_VIDEO_CodingVP8 ? NULL : kVP9ProfileLevels,
            codingType == OMX_VIDEO_CodingVP8 ?  0 : NELEM(kVP9ProfileLevels),
            320 /* width */, 240 /* height */, callbacks, appData, component),
      mMode(codingType == OMX_VIDEO_CodingVP8 ? MODE_VP8 : MODE_VP9),
      mEOSStatus(INPUT_DATA_AVAILABLE),
      mCtx(NULL),
      mFrameParallelMode(false),
      mTimeStampIdx(0),
      mImg(NULL) {
    // arbitrary from avc/hevc as vpx does not specify a min compression ratio
    const size_t kMinCompressionRatio = mMode == MODE_VP8 ? 2 : 4;
    const char *mime = mMode == MODE_VP8 ? MEDIA_MIMETYPE_VIDEO_VP8 : MEDIA_MIMETYPE_VIDEO_VP9;
    const size_t kMaxOutputBufferSize = 2560 * 2560 * 3 / 2;
    initPorts(
            kNumBuffers, kMaxOutputBufferSize / kMinCompressionRatio /* inputBufferSize */,
            kNumBuffers, mime, kMinCompressionRatio);
    ALOGE("calling constructor of GoldfishVPX");
    CHECK_EQ(initDecoder(), (status_t)OK);
}

GoldfishVPX::~GoldfishVPX() {
    ALOGE("calling destructor of GoldfishVPX");
    destroyDecoder();
}

bool GoldfishVPX::supportDescribeHdrStaticInfo() {
    return true;
}

bool GoldfishVPX::supportDescribeHdr10PlusInfo() {
    return true;
}

status_t GoldfishVPX::initDecoder() {
    mCtx = new vpx_codec_ctx_t;
    mCtx->vpversion = mMode == MODE_VP8 ? 8 : 9;

    int vpx_err = 0;
    if ((vpx_err = vpx_codec_dec_init(mCtx))) {
        ALOGE("vpx decoder failed to initialize. (%d)", vpx_err);
        return UNKNOWN_ERROR;
    }

    return OK;
}

status_t GoldfishVPX::destroyDecoder() {
    vpx_codec_destroy(mCtx);
    delete mCtx;
    mCtx = NULL;
    return OK;
}

void GoldfishVPX::setup_ctx_parameters(vpx_codec_ctx_t* ctx) {
    ctx->width = mWidth;
    ctx->height = mHeight;
    ctx->outputBufferWidth = outputBufferWidth();
    ctx->outputBufferHeight = outputBufferHeight();
    OMX_PARAM_PORTDEFINITIONTYPE *outDef = &editPortInfo(kOutputPortIndex)->mDef;
    int32_t bpp = (outDef->format.video.eColorFormat == OMX_COLOR_FormatYUV420Planar16) ? 2 : 1;
    ctx->bpp =  bpp;
}

bool GoldfishVPX::outputBuffers(bool flushDecoder, bool display, bool eos, bool *portWillReset) {
    List<BufferInfo *> &outQueue = getPortQueue(1);
    BufferInfo *outInfo = NULL;
    OMX_BUFFERHEADERTYPE *outHeader = NULL;

    if (flushDecoder && mFrameParallelMode) {
        // Flush decoder by passing NULL data ptr and 0 size.
        // Ideally, this should never fail.
        if (vpx_codec_flush(mCtx)) {
            ALOGE("Failed to flush on2 decoder.");
            return false;
        }
    }

    if (!display) {
        if (!flushDecoder) {
            ALOGE("Invalid operation.");
            return false;
        }
        // Drop all the decoded frames in decoder.
        // TODO: move this to host, with something like
        // vpx_codec_drop_all_frames(mCtx);
        setup_ctx_parameters(mCtx);
        while ((mImg = vpx_codec_get_frame(mCtx))) {
        }
        return true;
    }

    while (!outQueue.empty()) {
        if (mImg == NULL) {
            setup_ctx_parameters(mCtx);
            mImg = vpx_codec_get_frame(mCtx);
            if (mImg == NULL) {
                break;
            }
        }
        uint32_t width = mImg->d_w;
        uint32_t height = mImg->d_h;
        outInfo = *outQueue.begin();
        outHeader = outInfo->mHeader;
        CHECK(mImg->fmt == VPX_IMG_FMT_I420 || mImg->fmt == VPX_IMG_FMT_I42016);
        OMX_COLOR_FORMATTYPE outputColorFormat = OMX_COLOR_FormatYUV420Planar;
        int32_t bpp = 1;
        if (mImg->fmt == VPX_IMG_FMT_I42016) {
            outputColorFormat = OMX_COLOR_FormatYUV420Planar16;
            bpp = 2;
        }
        handlePortSettingsChange(portWillReset, width, height, outputColorFormat);
        if (*portWillReset) {
            return true;
        }

        outHeader->nOffset = 0;
        outHeader->nFlags = 0;
        outHeader->nFilledLen = (outputBufferWidth() * outputBufferHeight() * bpp * 3) / 2;
        PrivInfo *privInfo = (PrivInfo *)mImg->user_priv;
        outHeader->nTimeStamp = privInfo->mTimeStamp;
        if (privInfo->mHdr10PlusInfo != nullptr) {
            queueOutputFrameConfig(privInfo->mHdr10PlusInfo);
        }

        if (outputBufferSafe(outHeader)) {
            uint8_t *dst = outHeader->pBuffer;
            memcpy(dst, mCtx->dst, outHeader->nFilledLen);
        } else {
            outHeader->nFilledLen = 0;
        }

        mImg = NULL;
        outInfo->mOwnedByUs = false;
        outQueue.erase(outQueue.begin());
        outInfo = NULL;
        notifyFillBufferDone(outHeader);
        outHeader = NULL;
    }

    if (!eos) {
        return true;
    }

    if (!outQueue.empty()) {
        outInfo = *outQueue.begin();
        outQueue.erase(outQueue.begin());
        outHeader = outInfo->mHeader;
        outHeader->nTimeStamp = 0;
        outHeader->nFilledLen = 0;
        outHeader->nFlags = OMX_BUFFERFLAG_EOS;
        outInfo->mOwnedByUs = false;
        notifyFillBufferDone(outHeader);
        mEOSStatus = OUTPUT_FRAMES_FLUSHED;
    }
    return true;
}

bool GoldfishVPX::outputBufferSafe(OMX_BUFFERHEADERTYPE *outHeader) {
    uint32_t width = outputBufferWidth();
    uint32_t height = outputBufferHeight();
    uint64_t nFilledLen = width;
    nFilledLen *= height;
    if (nFilledLen > UINT32_MAX / 3) {
        ALOGE("b/29421675, nFilledLen overflow %llu w %u h %u",
                (unsigned long long)nFilledLen, width, height);
        android_errorWriteLog(0x534e4554, "29421675");
        return false;
    } else if (outHeader->nAllocLen < outHeader->nFilledLen) {
        ALOGE("b/27597103, buffer too small");
        android_errorWriteLog(0x534e4554, "27597103");
        return false;
    }

    return true;
}

void GoldfishVPX::onQueueFilled(OMX_U32 /* portIndex */) {
    if (mOutputPortSettingsChange != NONE || mEOSStatus == OUTPUT_FRAMES_FLUSHED) {
        return;
    }

    List<BufferInfo *> &inQueue = getPortQueue(0);
    List<BufferInfo *> &outQueue = getPortQueue(1);
    bool EOSseen = false;
    bool portWillReset = false;

    while ((mEOSStatus == INPUT_EOS_SEEN || !inQueue.empty())
            && !outQueue.empty()) {
        // Output the pending frames that left from last port reset or decoder flush.
        if (mEOSStatus == INPUT_EOS_SEEN || mImg != NULL) {
            if (!outputBuffers(
                     mEOSStatus == INPUT_EOS_SEEN, true /* display */,
                     mEOSStatus == INPUT_EOS_SEEN, &portWillReset)) {
                ALOGE("on2 decoder failed to output frame.");
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                return;
            }
            if (portWillReset || mEOSStatus == OUTPUT_FRAMES_FLUSHED ||
                    mEOSStatus == INPUT_EOS_SEEN) {
                return;
            }
            // Continue as outQueue may be empty now.
            continue;
        }

        BufferInfo *inInfo = *inQueue.begin();
        OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;

        // Software VP9 Decoder does not need the Codec Specific Data (CSD)
        // (specified in http://www.webmproject.org/vp9/profiles/). Ignore it if
        // it was passed.
        if (inHeader->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
            // Only ignore CSD buffer for VP9.
            if (mMode == MODE_VP9) {
                inQueue.erase(inQueue.begin());
                inInfo->mOwnedByUs = false;
                notifyEmptyBufferDone(inHeader);
                continue;
            } else {
                // Tolerate the CSD buffer for VP8. This is a workaround
                // for b/28689536.
                ALOGW("WARNING: Got CSD buffer for VP8.");
            }
        }

        mPrivInfo[mTimeStampIdx].mTimeStamp = inHeader->nTimeStamp;

        if (inInfo->mFrameConfig) {
            mPrivInfo[mTimeStampIdx].mHdr10PlusInfo = dequeueInputFrameConfig();
        } else {
            mPrivInfo[mTimeStampIdx].mHdr10PlusInfo.clear();
        }

        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            mEOSStatus = INPUT_EOS_SEEN;
            EOSseen = true;
        }

        if (inHeader->nFilledLen > 0) {
            int err = vpx_codec_decode(mCtx, inHeader->pBuffer + inHeader->nOffset,
                    inHeader->nFilledLen, &mPrivInfo[mTimeStampIdx], 0);
            if (err == VPX_CODEC_OK) {
                inInfo->mOwnedByUs = false;
                inQueue.erase(inQueue.begin());
                inInfo = NULL;
                notifyEmptyBufferDone(inHeader);
                inHeader = NULL;
            } else {
                ALOGE("on2 decoder failed to decode frame. err: %d", err);
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                return;
            }
        }

        mTimeStampIdx = (mTimeStampIdx + 1) % kNumBuffers;

        if (!outputBuffers(
                 EOSseen /* flushDecoder */, true /* display */, EOSseen, &portWillReset)) {
            ALOGE("on2 decoder failed to output frame.");
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            return;
        }
        if (portWillReset) {
            return;
        }
    }
}

void GoldfishVPX::onPortFlushCompleted(OMX_U32 portIndex) {
    if (portIndex == kInputPortIndex) {
        bool portWillReset = false;
        if (!outputBuffers(
                 true /* flushDecoder */, false /* display */, false /* eos */, &portWillReset)) {
            ALOGE("Failed to flush decoder.");
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            return;
        }
        mEOSStatus = INPUT_DATA_AVAILABLE;
    }
}

void GoldfishVPX::onReset() {
    bool portWillReset = false;
    if (!outputBuffers(
             true /* flushDecoder */, false /* display */, false /* eos */, &portWillReset)) {
        ALOGW("Failed to flush decoder. Try to hard reset decoder");
        destroyDecoder();
        initDecoder();
    }
    mEOSStatus = INPUT_DATA_AVAILABLE;
}

}  // namespace android

android::GoldfishOMXComponent *createGoldfishOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    if (!strcmp(name, "OMX.google.goldfish.vp8.decoder")) {
        return new android::GoldfishVPX(
                name, "video_decoder.vp8", OMX_VIDEO_CodingVP8,
                callbacks, appData, component);
    } else if (!strcmp(name, "OMX.google.goldfish.vp9.decoder")) {
        return new android::GoldfishVPX(
                name, "video_decoder.vp9", OMX_VIDEO_CodingVP9,
                callbacks, appData, component);
    } else {
        CHECK(!"Unknown component");
    }
    return NULL;
}
