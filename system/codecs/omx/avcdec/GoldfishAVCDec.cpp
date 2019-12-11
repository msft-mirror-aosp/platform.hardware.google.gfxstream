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

//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include "GoldfishAVCDec.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <OMX_VideoExt.h>
#include <inttypes.h>

namespace android {

#define componentName                   "video_decoder.avc"
#define codingType                      OMX_VIDEO_CodingAVC
#define CODEC_MIME_TYPE                 MEDIA_MIMETYPE_VIDEO_AVC

/** Function and structure definitions to keep code similar for each codec */
#define ivdec_api_function              ih264d_api_function
#define ivdext_create_ip_t              ih264d_create_ip_t
#define ivdext_create_op_t              ih264d_create_op_t
#define ivdext_delete_ip_t              ih264d_delete_ip_t
#define ivdext_delete_op_t              ih264d_delete_op_t
#define ivdext_ctl_set_num_cores_ip_t   ih264d_ctl_set_num_cores_ip_t
#define ivdext_ctl_set_num_cores_op_t   ih264d_ctl_set_num_cores_op_t

#define IVDEXT_CMD_CTL_SET_NUM_CORES    \
        (IVD_CONTROL_API_COMMAND_TYPE_T)IH264D_CMD_CTL_SET_NUM_CORES

static const CodecProfileLevel kProfileLevels[] = {
    { OMX_VIDEO_AVCProfileConstrainedBaseline, OMX_VIDEO_AVCLevel52 },

    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel52 },

    { OMX_VIDEO_AVCProfileMain,     OMX_VIDEO_AVCLevel52 },

    { OMX_VIDEO_AVCProfileConstrainedHigh,     OMX_VIDEO_AVCLevel52 },

    { OMX_VIDEO_AVCProfileHigh,     OMX_VIDEO_AVCLevel52 },
};

GoldfishAVCDec::GoldfishAVCDec(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : GoldfishVideoDecoderOMXComponent(
            name, componentName, codingType,
            kProfileLevels, ARRAY_SIZE(kProfileLevels),
            320 /* width */, 240 /* height */, callbacks,
            appData, component),
      mOmxColorFormat(OMX_COLOR_FormatYUV420Planar),
      mChangingResolution(false),
      mSignalledError(false),
      mInputOffset(0){
    initPorts(
            1 /* numMinInputBuffers */, kNumBuffers, INPUT_BUF_SIZE,
            1 /* numMinOutputBuffers */, kNumBuffers, CODEC_MIME_TYPE);

    mTimeStart = mTimeEnd = systemTime();

    // If input dump is enabled, then open create an empty file
    GENERATE_FILE_NAMES();
    CREATE_DUMP_FILE(mInFile);
}

GoldfishAVCDec::~GoldfishAVCDec() {
    CHECK_EQ(deInitDecoder(), (status_t)OK);
}

void GoldfishAVCDec::logVersion() {
    // TODO: get emulation decoder implementation version from the host.
    ALOGV("GoldfishAVC decoder version 1.0");
}

status_t GoldfishAVCDec::resetPlugin() {
    mIsInFlush = false;
    mReceivedEOS = false;

    memset(mTimeStamps, 0, sizeof(mTimeStamps));
    memset(mTimeStampsValid, 0, sizeof(mTimeStampsValid));

    /* Initialize both start and end times */
    mTimeStart = mTimeEnd = systemTime();

    return OK;
}

status_t GoldfishAVCDec::resetDecoder() {
    // The resolution may have changed, so our safest bet is to just destroy the
    // current context and recreate another one, with the new width and height.
    mContext->destroyH264Context();
    mContext->initH264Context(mWidth,
                              mHeight,
                              outputBufferWidth(),
                              outputBufferHeight(),
                              MediaH264Decoder::PixelFormat::YUV420P);

    return OK;
}

status_t GoldfishAVCDec::setFlushMode() {
    /* Set the decoder in Flush mode, subsequent decode() calls will flush */
    mContext->flush();

    mIsInFlush = true;
    return OK;
}

status_t GoldfishAVCDec::initDecoder() {
    /* Initialize the decoder */
    mContext.reset(new MediaH264Decoder());
    mContext->initH264Context(mWidth,
                              mHeight,
                              outputBufferWidth(),
                              outputBufferHeight(),
                              MediaH264Decoder::PixelFormat::YUV420P);

    /* Reset the plugin state */
    resetPlugin();

    /* Get codec version */
    logVersion();

    return OK;
}

status_t GoldfishAVCDec::deInitDecoder() {
    if (mContext) {
        mContext->destroyH264Context();
        mContext.reset();
    }

    mChangingResolution = false;

    return OK;
}

void GoldfishAVCDec::onReset() {
    GoldfishVideoDecoderOMXComponent::onReset();

    mSignalledError = false;
    mInputOffset = 0;
    resetDecoder();
    resetPlugin();
}

bool GoldfishAVCDec::getVUIParams() {
    ALOGE("%s: NOT IMPLEMENTED", __func__);
    /*
    IV_API_CALL_STATUS_T status;
    ih264d_ctl_get_vui_params_ip_t s_ctl_get_vui_params_ip;
    ih264d_ctl_get_vui_params_op_t s_ctl_get_vui_params_op;

    s_ctl_get_vui_params_ip.e_cmd = IVD_CMD_VIDEO_CTL;
    s_ctl_get_vui_params_ip.e_sub_cmd =
        (IVD_CONTROL_API_COMMAND_TYPE_T)IH264D_CMD_CTL_GET_VUI_PARAMS;

    s_ctl_get_vui_params_ip.u4_size =
        sizeof(ih264d_ctl_get_vui_params_ip_t);

    s_ctl_get_vui_params_op.u4_size = sizeof(ih264d_ctl_get_vui_params_op_t);

    status = ivdec_api_function(
            (iv_obj_t *)mCodecCtx, (void *)&s_ctl_get_vui_params_ip,
            (void *)&s_ctl_get_vui_params_op);

    if (status != IV_SUCCESS) {
        ALOGW("Error in getting VUI params: 0x%x",
                s_ctl_get_vui_params_op.u4_error_code);
        return false;
    }

    int32_t primaries = s_ctl_get_vui_params_op.u1_colour_primaries;
    int32_t transfer = s_ctl_get_vui_params_op.u1_tfr_chars;
    int32_t coeffs = s_ctl_get_vui_params_op.u1_matrix_coeffs;
    bool fullRange = s_ctl_get_vui_params_op.u1_video_full_range_flag;

    ColorAspects colorAspects;
    ColorUtils::convertIsoColorAspectsToCodecAspects(
            primaries, transfer, coeffs, fullRange, colorAspects);

    // Update color aspects if necessary.
    if (colorAspectsDiffer(colorAspects, mBitstreamColorAspects)) {
        mBitstreamColorAspects = colorAspects;
        status_t err = handleColorAspectsChange();
        CHECK(err == OK);
    }
    return true;
    */
    return false;
}

bool GoldfishAVCDec::setDecodeArgs(
        OMX_BUFFERHEADERTYPE *inHeader,
        OMX_BUFFERHEADERTYPE *outHeader,
        size_t timeStampIx) {
    size_t sizeY = outputBufferWidth() * outputBufferHeight();
    size_t sizeUV = sizeY / 4;

    /* When in flush and after EOS with zero byte input,
     * inHeader is set to zero. Hence check for non-null */
    if (inHeader) {
        mCurrentTs = timeStampIx;
        mConsumedBytes = inHeader->nFilledLen - mInputOffset;
        mInPBuffer = inHeader->pBuffer + inHeader->nOffset + mInputOffset;
    } else {
        mCurrentTs = 0;
        mConsumedBytes = 0;
        mInPBuffer = nullptr;
    }

    if (outHeader) {
        if (outHeader->nAllocLen < sizeY + (sizeUV * 2)) {
            android_errorWriteLog(0x534e4554, "27833616");
            return false;
        }
        mOutHeaderBuf = outHeader->pBuffer;
    } else {
        // We flush out on the host side
        mOutHeaderBuf = nullptr;
    }

    return true;
}

void GoldfishAVCDec::onPortFlushCompleted(OMX_U32 portIndex) {
    /* Once the output buffers are flushed, ignore any buffers that are held in decoder */
    if (kOutputPortIndex == portIndex) {
        setFlushMode();
        resetPlugin();
    } else {
        mInputOffset = 0;
    }
}

void GoldfishAVCDec::onQueueFilled(OMX_U32 portIndex) {
    UNUSED(portIndex);
    OMX_BUFFERHEADERTYPE *inHeader = NULL;
    BufferInfo *inInfo = NULL;

    if (mSignalledError) {
        return;
    }
    if (mOutputPortSettingsChange != NONE) {
        return;
    }

    if (mContext == nullptr) {
        if (OK != initDecoder()) {
            ALOGE("Failed to initialize decoder");
            notify(OMX_EventError, OMX_ErrorUnsupportedSetting, 0, NULL);
            mSignalledError = true;
            return;
        }
    }

    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    while (!outQueue.empty()) {
        BufferInfo *outInfo;
        OMX_BUFFERHEADERTYPE *outHeader;
        size_t timeStampIx = 0;

        if (!mIsInFlush && (NULL == inHeader)) {
            if (!inQueue.empty()) {
                inInfo = *inQueue.begin();
                inHeader = inInfo->mHeader;
                if (inHeader == NULL) {
                    inQueue.erase(inQueue.begin());
                    inInfo->mOwnedByUs = false;
                    continue;
                }
            } else {
                break;
            }
        }

        outInfo = *outQueue.begin();
        outHeader = outInfo->mHeader;
        outHeader->nFlags = 0;
        outHeader->nTimeStamp = 0;
        outHeader->nOffset = 0;

        if (inHeader != NULL) {
            if (inHeader->nFilledLen == 0) {
                // An empty buffer can be end of stream (EOS) buffer, so
                // we'll set the decoder in flush mode if so. If it's not EOS,
                // then just release the buffer.
                inQueue.erase(inQueue.begin());
                inInfo->mOwnedByUs = false;
                notifyEmptyBufferDone(inHeader);

                if (!(inHeader->nFlags & OMX_BUFFERFLAG_EOS)) {
                    return;
                }

                mReceivedEOS = true;
                inHeader = NULL;
                setFlushMode();
            } else if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
                mReceivedEOS = true;
            }
        }

        /* Get a free slot in timestamp array to hold input timestamp */
        {
            size_t i;
            timeStampIx = 0;
            for (i = 0; i < MAX_TIME_STAMPS; i++) {
                if (!mTimeStampsValid[i]) {
                    timeStampIx = i;
                    break;
                }
            }
            if (inHeader != NULL) {
                mTimeStampsValid[timeStampIx] = true;
                mTimeStamps[timeStampIx] = inHeader->nTimeStamp;
            }
        }

        {
            nsecs_t timeDelay, timeTaken;

            if (!setDecodeArgs(inHeader, outHeader, timeStampIx)) {
                ALOGE("Decoder arg setup failed");
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }

            mTimeStart = systemTime();
            /* Compute time elapsed between end of previous decode()
             * to start of current decode() */
            timeDelay = mTimeStart - mTimeEnd;

            // TODO: We also need to send the timestamp
            h264_result_t h264Res = {(int)MediaH264Decoder::Err::NoErr, 0};
            if (inHeader != nullptr) {
                ALOGE("Decoding frame(sz=%lu)", (unsigned long)(inHeader->nFilledLen - mInputOffset));
                h264Res = mContext->decodeFrame(mInPBuffer,
                                                inHeader->nFilledLen - mInputOffset);
                mConsumedBytes = h264Res.bytesProcessed;
                if (h264Res.ret == (int)MediaH264Decoder::Err::DecoderRestarted) {
                    // The host will always restart when given a new set of SPS
                    // and PPS frames.
                    mChangingResolution = true;
                }
            } else {
                ALOGE("No more input data. Attempting to get a decoded frame, if any.");
            }
            h264_image_t img = mContext->getImage();


            // TODO: support getVUIParams()
//            getVUIParams();

            mTimeEnd = systemTime();
            /* Compute time taken for decode() */
            timeTaken = mTimeEnd - mTimeStart;

            ALOGV("timeTaken=%6lldus delay=%6lldus numBytes=Nan",
                    (long long) (timeTaken / 1000LL), (long long) (timeDelay / 1000LL));

            if ((inHeader != NULL) && (img.data == nullptr)) {
                /* If the input did not contain picture data, then ignore
                 * the associated timestamp */
                mTimeStampsValid[timeStampIx] = false;
            }

            // If the decoder is in the changing resolution mode and there is no output present,
            // that means the switching is done and it's ready to reset the decoder and the plugin.
            if (mChangingResolution && img.data == nullptr) {
                mChangingResolution = false;
                resetPlugin();
                // The decoder on the host has actually already restarted given
                // the new information, so we don't have to refeed the same
                // information again.
                mInputOffset += mConsumedBytes;
                continue;
            }

            // Combine the resolution change and coloraspects change in one
            // PortSettingChange event if necessary.
            if (img.data != nullptr) {
                bool portWillReset = false;
                handlePortSettingsChange(&portWillReset, img.width, img.height);
                if (portWillReset) {
                    ALOGE("port resetting (img.width=%u, img.height=%u, mWidth=%u, mHeight=%u)",
                          img.width, img.height, mWidth, mHeight);
                    resetDecoder();
                    resetPlugin();
                    return;
                }
            }

            if (img.data != nullptr) {
                outHeader->nFilledLen = img.ret;
                memcpy(outHeader->pBuffer, img.data, img.ret);
                outHeader->nTimeStamp = mTimeStamps[mCurrentTs];
                mTimeStampsValid[mCurrentTs] = false;

                outInfo->mOwnedByUs = false;
                outQueue.erase(outQueue.begin());
                outInfo = NULL;
                notifyFillBufferDone(outHeader);
                outHeader = NULL;
            } else if (mIsInFlush) {
                /* If in flush mode and no output is returned by the codec,
                 * then come out of flush mode */
                mIsInFlush = false;

                /* If EOS was recieved on input port and there is no output
                 * from the codec, then signal EOS on output port */
                if (mReceivedEOS) {
                    outHeader->nFilledLen = 0;
                    outHeader->nFlags |= OMX_BUFFERFLAG_EOS;

                    outInfo->mOwnedByUs = false;
                    outQueue.erase(outQueue.begin());
                    outInfo = NULL;
                    notifyFillBufferDone(outHeader);
                    outHeader = NULL;
                    resetPlugin();
                }
            }
            mInputOffset += mConsumedBytes;
        }

        // If more than 4 bytes are remaining in input, then do not release it
        if (inHeader != NULL && ((inHeader->nFilledLen - mInputOffset) <= 4)) {
            inInfo->mOwnedByUs = false;
            inQueue.erase(inQueue.begin());
            inInfo = NULL;
            notifyEmptyBufferDone(inHeader);
            inHeader = NULL;
            mInputOffset = 0;

            /* If input EOS is seen and decoder is not in flush mode,
             * set the decoder in flush mode.
             * There can be a case where EOS is sent along with last picture data
             * In that case, only after decoding that input data, decoder has to be
             * put in flush. This case is handled here  */

            if (mReceivedEOS && !mIsInFlush) {
                setFlushMode();
            }
        }
    }
}

int GoldfishAVCDec::getColorAspectPreference() {
    return kPreferBitstream;
}

}  // namespace android

android::GoldfishOMXComponent *createGoldfishOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks, OMX_PTR appData,
        OMX_COMPONENTTYPE **component) {
    return new android::GoldfishAVCDec(name, callbacks, appData, component);
}

