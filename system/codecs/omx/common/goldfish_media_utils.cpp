/*
 * Copyright (C) 2018 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "goldfish_media_utils.h"

#include "goldfish_address_space.h"

#include <log/log.h>
#include <memory>


namespace {

std::unique_ptr<GoldfishMediaTransport> sTransport;

class GoldfishMediaTransportImpl : public GoldfishMediaTransport {
public:
    GoldfishMediaTransportImpl();
    ~GoldfishMediaTransportImpl();

    virtual void writeParam(__u64 val, unsigned int num) override;
    virtual bool sendOperation(MediaCodecType type, MediaOperation op) override;
    virtual uint8_t* getInputAddr() const override;
    virtual uint8_t* getOutputAddr() const override;
    virtual uint8_t* getReturnAddr() const override;
    virtual __u64 offsetOf(uint64_t addr) const override;

private:
    address_space_handle_t mHandle;
    uint64_t  mOffset;
    uint64_t  mPhysAddr;
    uint64_t  mSize;
    void* mStartPtr = nullptr;

    // MediaCodecType will be or'd together with the metadata, so the highest 8-bits
    // will have the type.
    static __u64 makeMetadata(MediaCodecType type,
                              MediaOperation op);

    // Chunk size for parameters/return data
    static constexpr size_t kParamSizeBytes = 4096; // 4K
    // Chunk size for input
    static constexpr size_t kInputSizeBytes = 4096 * 4096; // 16M
    // Chunk size for output
    static constexpr size_t kOutputSizeBytes = 4096 * 4096; // 16M
    // Maximum number of parameters that can be passed
    static constexpr size_t kMaxParams = 32;
    // Offset from the memory region for return data (8 is size of
    // a parameter in bytes)
    static constexpr size_t kReturnOffset = 8 * kMaxParams;
};
}  // namespace

GoldfishMediaTransportImpl::~GoldfishMediaTransportImpl() {
  if(mHandle >= 0) {
    goldfish_address_space_close(mHandle);
    mHandle = -1;
  }
}

GoldfishMediaTransportImpl::GoldfishMediaTransportImpl() {
    // Allocate host memory; the contiguous memory region will be laid out as
    // follows:
    // ========================================================
    // | kParamSizeBytes | kInputSizeBytes | kOutputSizeBytes |
    // ========================================================
    mHandle = goldfish_address_space_open();
    if (mHandle < 0) {
        ALOGE("Failed to ping host to allocate memory");
        abort();
    }
    mSize = kParamSizeBytes + kInputSizeBytes + kOutputSizeBytes;
    bool success = goldfish_address_space_allocate(mHandle, mSize, &mPhysAddr, &mOffset);
    if (success) {
        ALOGD("successfully allocated %d bytes in goldfish_address_block", (int)mSize);
        mStartPtr = goldfish_address_space_map(mHandle, mOffset, mSize);
        ALOGD("guest address is %p", mStartPtr);

        struct goldfish_address_space_ping pingInfo;
        pingInfo.metadata = GoldfishAddressSpaceSubdeviceType::Media;
        pingInfo.offset = mOffset;
        if (goldfish_address_space_ping(mHandle, &pingInfo) == false) {
            ALOGE("Failed to ping host to allocate memory");
            abort();
            return;
        } else {
            ALOGD("successfully pinged host to allocate memory");
        }
    } else {
        ALOGE("failed to allocate %d bytes in goldfish_address_block", (int)mSize);
        abort();
    }
}

// static
GoldfishMediaTransport* GoldfishMediaTransport::getInstance() {
    if (sTransport == nullptr) {
        sTransport.reset(new GoldfishMediaTransportImpl());
    }
    return sTransport.get();
}

// static
__u64 GoldfishMediaTransportImpl::makeMetadata(MediaCodecType type,
                                               MediaOperation op) {
    // Shift |type| into the highest 8-bits, leaving the lower bits for other
    // metadata.
    return ((__u64)type << (64 - 8)) | static_cast<uint8_t>(op);
}

uint8_t* GoldfishMediaTransportImpl::getInputAddr() const {
    return (uint8_t*)mStartPtr + kParamSizeBytes;
}

uint8_t* GoldfishMediaTransportImpl::getOutputAddr() const {
    return getInputAddr() + kInputSizeBytes;
}

uint8_t* GoldfishMediaTransportImpl::getReturnAddr() const {
    return (uint8_t*)mStartPtr + kReturnOffset;
}

__u64 GoldfishMediaTransportImpl::offsetOf(uint64_t addr) const {
    return addr - (uint64_t)mStartPtr;
}

void GoldfishMediaTransportImpl::writeParam(__u64 val, unsigned int num) {
    uint8_t* p = (uint8_t*)mStartPtr;
    uint64_t* pint = (uint64_t*)(p + 8 * num);
    *pint = val;
}

bool GoldfishMediaTransportImpl::sendOperation(MediaCodecType type,
                                               MediaOperation op) {
    struct goldfish_address_space_ping pingInfo;
    pingInfo.metadata = makeMetadata(type, op);
    pingInfo.offset = mOffset;
    if (goldfish_address_space_ping(mHandle, &pingInfo) == false) {
        ALOGE("failed to ping host");
        abort();
        return false;
    } else {
        ALOGD("successfully pinged host for operation type=%d, op=%d", (int)type, (int)op);
    }

    return true;
}
