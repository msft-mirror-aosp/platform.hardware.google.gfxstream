#include <log/log.h>

#include <linux/types.h>
#include <linux/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <string>
#include <errno.h>
#include "goldfish_vpx_defs.h"
#include "goldfish_media_utils.h"

static vpx_image_t myImg;

static void sendVpxOperation(vpx_codec_ctx_t* ctx, MediaOperation op) {
    auto transport = GoldfishMediaTransport::getInstance();
    transport->sendOperation(
            ctx->vpversion == 8 ?
                MediaCodecType::VP8Codec :
                MediaCodecType::VP9Codec,
            op);
}

int vpx_codec_destroy(vpx_codec_ctx_t* ctx) {
    sendVpxOperation(ctx, MediaOperation::DestroyContext);
    return 0;
}

int vpx_codec_dec_init(vpx_codec_ctx_t* ctx) {
    auto transport = GoldfishMediaTransport::getInstance();
    // data and dst are on the host side actually
    ctx->data = transport->getInputAddr();
    ctx->dst = transport->getOutputAddr();
    sendVpxOperation(ctx, MediaOperation::InitContext);
    return 0;
}

static int getReturnCode(uint8_t* ptr) {
    int* pint = (int*)(ptr);
    return *pint;
}

static void getVpxFrame(uint8_t* ptr) {
    uint8_t* imgptr = (ptr + 8);
    myImg.fmt = *(vpx_img_fmt_t*)imgptr;
    imgptr += 8;
    myImg.d_w = *(unsigned int *)imgptr;
    imgptr += 8;
    myImg.d_h = *(unsigned int *)imgptr;
    imgptr += 8;
    myImg.user_priv = (void*)(*(uint64_t*)imgptr);
}

//TODO: we might not need to do the putting all the time
vpx_image_t* vpx_codec_get_frame(vpx_codec_ctx_t* ctx) {
    auto transport = GoldfishMediaTransport::getInstance();

    transport->writeParam(ctx->outputBufferWidth, 0);
    transport->writeParam(ctx->outputBufferHeight, 1);
    transport->writeParam(ctx->width, 2);
    transport->writeParam(ctx->height, 3);
    transport->writeParam(ctx->bpp, 4);
    transport->writeParam(transport->offsetOf((uint64_t)(ctx->dst)), 5);

    sendVpxOperation(ctx, MediaOperation::GetImage);

    auto* retptr = transport->getReturnAddr();
    int ret = getReturnCode(retptr);
    if (ret) {
        return nullptr;
    }
    getVpxFrame(retptr);
    return &myImg;
}

int vpx_codec_flush(vpx_codec_ctx_t* ctx) {
    sendVpxOperation(ctx, MediaOperation::Flush);
    return 0;
}

int vpx_codec_decode(vpx_codec_ctx_t *ctx,
                     const uint8_t* data,
                     unsigned int data_sz,
                     void* user_priv,
                     long deadline) {
    auto transport = GoldfishMediaTransport::getInstance();
    memcpy(ctx->data, data, data_sz);

    transport->writeParam(transport->offsetOf((uint64_t)(ctx->data)), 0);
    transport->writeParam((__u64)data_sz, 1);
    transport->writeParam((__u64)user_priv, 2);
    sendVpxOperation(ctx, MediaOperation::DecodeImage);
    return 0;
}
