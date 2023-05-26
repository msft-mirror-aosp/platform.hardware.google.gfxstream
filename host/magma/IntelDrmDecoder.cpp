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

#include "host/magma/IntelDrmDecoder.h"

#include <i915_drm.h>
#include <magma_intel_gen_defs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "RenderThreadInfoMagma.h"
#include "host/magma/DrmDevice.h"

namespace gfxstream {
namespace magma {

std::unique_ptr<IntelDrmDecoder> IntelDrmDecoder::create(uint32_t context_id) {
    std::unique_ptr<IntelDrmDecoder> decoder(new IntelDrmDecoder());
    decoder->mContextId = context_id;
    INFO("IntelDrmDecoder created for context %" PRIu32, context_id);
    return decoder;
}

#define MAGMA_DECODER_BIND_METHOD(method)                               \
    magma_server_context_t::method = [](auto... args) {                 \
        auto decoder = RenderThreadInfoMagma::get() -> mMagmaDec.get(); \
        return static_cast<IntelDrmDecoder*>(decoder)->method(args...); \
    }

IntelDrmDecoder::IntelDrmDecoder() : Decoder() {
    MAGMA_DECODER_BIND_METHOD(magma_device_import);
    MAGMA_DECODER_BIND_METHOD(magma_device_release);
    MAGMA_DECODER_BIND_METHOD(magma_device_query);
    MAGMA_DECODER_BIND_METHOD(magma_device_create_connection);
    MAGMA_DECODER_BIND_METHOD(magma_connection_release);
    MAGMA_DECODER_BIND_METHOD(magma_connection_create_buffer);
    MAGMA_DECODER_BIND_METHOD(magma_connection_release_buffer);
    MAGMA_DECODER_BIND_METHOD(magma_connection_create_semaphore);
    MAGMA_DECODER_BIND_METHOD(magma_connection_release_semaphore);
    MAGMA_DECODER_BIND_METHOD(magma_buffer_export);
    MAGMA_DECODER_BIND_METHOD(magma_semaphore_signal);
    MAGMA_DECODER_BIND_METHOD(magma_semaphore_reset);
    MAGMA_DECODER_BIND_METHOD(magma_poll);
    MAGMA_DECODER_BIND_METHOD(magma_connection_get_error);
    MAGMA_DECODER_BIND_METHOD(magma_connection_create_context);
    MAGMA_DECODER_BIND_METHOD(magma_connection_release_context);
    MAGMA_DECODER_BIND_METHOD(magma_connection_map_buffer);
    MAGMA_DECODER_BIND_METHOD(magma_connection_unmap_buffer);
}

magma_status_t IntelDrmDecoder::magma_device_import(magma_handle_t device_channel,
                                                    magma_device_t* device_out) {
    *device_out = 0;
    auto device = DrmDevice::create();
    if (!device) {
        return MAGMA_STATUS_INTERNAL_ERROR;
    }
    *device_out = mDevices.create(std::move(*device));
    INFO("magma_device_import() -> %" PRIu64, *device_out);
    return MAGMA_STATUS_OK;
}

void IntelDrmDecoder::magma_device_release(magma_device_t device) {
    INFO("magma_device_release(%" PRIu64 ")", device);
}

magma_status_t IntelDrmDecoder::magma_device_query(magma_device_t device, uint64_t id,
                                                   magma_handle_t* result_buffer_out,
                                                   uint64_t* result_out) {
    *result_buffer_out = MAGMA_INVALID_OBJECT_ID;
    *result_out = 0;

    auto dev = mDevices.get(device);
    if (!dev) {
        return MAGMA_STATUS_INVALID_ARGS;
    }

    // TODO(b/275093891): query or standardize hard-coded values
    constexpr uint32_t kExtraPageCount = 9;
    constexpr uint64_t kIntelTimestampRegisterOffset = 0x23f8;
    switch (id) {
        case MAGMA_QUERY_VENDOR_ID: {
            *result_out = MAGMA_VENDOR_ID_INTEL;
            return MAGMA_STATUS_OK;
        }
        case MAGMA_QUERY_DEVICE_ID: {
            auto result = dev->getParam(I915_PARAM_CHIPSET_ID);
            if (!result) {
                return MAGMA_STATUS_INTERNAL_ERROR;
            }
            *result_out = *result;
            return MAGMA_STATUS_OK;
        }
        case MAGMA_QUERY_IS_TOTAL_TIME_SUPPORTED: {
            *result_out = 0;
            return MAGMA_STATUS_OK;
        }
        case kMagmaIntelGenQuerySubsliceAndEuTotal: {
            auto subslice_result = dev->getParam(I915_PARAM_SUBSLICE_TOTAL);
            auto eu_result = dev->getParam(I915_PARAM_EU_TOTAL);
            if (!subslice_result || !eu_result) {
                return MAGMA_STATUS_INTERNAL_ERROR;
            }
            *result_out = (static_cast<uint64_t>(*subslice_result) << 32) | *eu_result;
            return MAGMA_STATUS_OK;
        }
        case kMagmaIntelGenQueryGttSize: {
            // GTT is synonymous with Aperture
            drm_i915_gem_get_aperture aperture{};
            int result = dev->ioctl(DRM_IOCTL_I915_GEM_GET_APERTURE, &aperture);
            if (result != 0) {
                ERR("DRM_IOCTL_I915_GEM_GET_APERTURE failed: %d", result);
                return MAGMA_STATUS_INTERNAL_ERROR;
            }
            *result_out = aperture.aper_available_size;
            return MAGMA_STATUS_OK;
        }
        case kMagmaIntelGenQueryExtraPageCount: {
            *result_out = kExtraPageCount;
            return MAGMA_STATUS_OK;
        }
        case kMagmaIntelGenQueryTimestamp: {
            WARN("kMagmaIntelGenQueryTimestamp not implemented");
            return MAGMA_STATUS_UNIMPLEMENTED;
        }
        case kMagmaIntelGenQueryTopology: {
            WARN("kMagmaIntelGenQueryTopology not implemented");
            return MAGMA_STATUS_UNIMPLEMENTED;
        }
        case kMagmaIntelGenQueryHasContextIsolation: {
            auto result = dev->getParam(I915_PARAM_HAS_CONTEXT_ISOLATION);
            if (!result) {
                return MAGMA_STATUS_INTERNAL_ERROR;
            }
            *result_out = *result;
            return MAGMA_STATUS_OK;
        }
        case kMagmaIntelGenQueryTimestampFrequency: {
            auto result = dev->getParam(I915_PARAM_CS_TIMESTAMP_FREQUENCY);
            if (!result) {
                return MAGMA_STATUS_INTERNAL_ERROR;
            }
            *result_out = *result;
            return MAGMA_STATUS_OK;
        }
        default: {
            return MAGMA_STATUS_INVALID_ARGS;
        }
    }
}

magma_status_t IntelDrmDecoder::magma_device_create_connection(magma_device_t device,
                                                               magma_connection_t* connection_out) {
    *connection_out = MAGMA_INVALID_OBJECT_ID;
    WARN("%s not implemented", __FUNCTION__);
    return MAGMA_STATUS_UNIMPLEMENTED;
}

void IntelDrmDecoder::magma_connection_release(magma_connection_t connection) {
    WARN("%s not implemented", __FUNCTION__);
}

magma_status_t IntelDrmDecoder::magma_connection_create_buffer(magma_connection_t connection,
                                                               uint64_t size, uint64_t* size_out,
                                                               magma_buffer_t* buffer_out,
                                                               magma_buffer_id_t* id_out) {
    *size_out = 0;
    *buffer_out = MAGMA_INVALID_OBJECT_ID;
    *id_out = MAGMA_INVALID_OBJECT_ID;
    WARN("%s not implemented", __FUNCTION__);
    return MAGMA_STATUS_UNIMPLEMENTED;
}

void IntelDrmDecoder::magma_connection_release_buffer(magma_connection_t connection,
                                                      magma_buffer_t buffer) {
    WARN("%s not implemented", __FUNCTION__);
}

magma_status_t IntelDrmDecoder::magma_connection_create_semaphore(
    magma_connection_t magma_connection, magma_semaphore_t* semaphore_out,
    magma_semaphore_id_t* id_out) {
    *semaphore_out = MAGMA_INVALID_OBJECT_ID;
    *id_out = MAGMA_INVALID_OBJECT_ID;
    WARN("%s not implemented", __FUNCTION__);
    return MAGMA_STATUS_UNIMPLEMENTED;
}

void IntelDrmDecoder::magma_connection_release_semaphore(magma_connection_t connection,
                                                         magma_semaphore_t semaphore) {
    WARN("%s not implemented", __FUNCTION__);
}

magma_status_t IntelDrmDecoder::magma_buffer_export(magma_buffer_t buffer,
                                                    magma_handle_t* buffer_handle_out) {
    *buffer_handle_out = MAGMA_INVALID_OBJECT_ID;
    WARN("%s not implemented", __FUNCTION__);
    return MAGMA_STATUS_UNIMPLEMENTED;
}

void IntelDrmDecoder::magma_semaphore_signal(magma_semaphore_t semaphore) {
    WARN("%s not implemented", __FUNCTION__);
}

void IntelDrmDecoder::magma_semaphore_reset(magma_semaphore_t semaphore) {
    WARN("%s not implemented", __FUNCTION__);
}

magma_status_t IntelDrmDecoder::magma_poll(magma_poll_item_t* items, uint32_t count,
                                           uint64_t timeout_ns) {
    WARN("%s not implemented", __FUNCTION__);
    return MAGMA_STATUS_UNIMPLEMENTED;
}

magma_status_t IntelDrmDecoder::magma_connection_get_error(magma_connection_t connection) {
    WARN("%s not implemented", __FUNCTION__);
    return MAGMA_STATUS_UNIMPLEMENTED;
}

magma_status_t IntelDrmDecoder::magma_connection_create_context(magma_connection_t connection,
                                                                uint32_t* context_id_out) {
    *context_id_out = MAGMA_INVALID_OBJECT_ID;
    WARN("%s not implemented", __FUNCTION__);
    return MAGMA_STATUS_UNIMPLEMENTED;
}

void IntelDrmDecoder::magma_connection_release_context(magma_connection_t connection,
                                                       uint32_t context_id) {
    WARN("%s not implemented", __FUNCTION__);
}

magma_status_t IntelDrmDecoder::magma_connection_map_buffer(magma_connection_t connection,
                                                            uint64_t hw_va, magma_buffer_t buffer,
                                                            uint64_t offset, uint64_t length,
                                                            uint64_t map_flags) {
    WARN("%s not implemented", __FUNCTION__);
    return MAGMA_STATUS_UNIMPLEMENTED;
}

void IntelDrmDecoder::magma_connection_unmap_buffer(magma_connection_t connection, uint64_t hw_va,
                                                    magma_buffer_t buffer) {
    WARN("%s not implemented", __FUNCTION__);
}

}  // namespace magma
}  // namespace gfxstream
