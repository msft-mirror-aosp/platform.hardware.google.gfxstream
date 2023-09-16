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

#include "VirtioGpuAddressSpaceStream.h"

#include "util.h"

address_space_handle_t virtgpu_address_space_open() {
    return (address_space_handle_t)(-EINVAL);
}

void virtgpu_address_space_close(address_space_handle_t) {
    // Handle opened by VirtioGpuDevice wrapper
}

bool virtgpu_address_space_ping(address_space_handle_t, struct address_space_ping* info) {
    int ret;
    struct VirtGpuExecBuffer exec = {};
    VirtGpuDevice* instance = VirtGpuDevice::getInstance();
    struct gfxstreamContextPing ping = {};

    ping.hdr.opCode = GFXSTREAM_CONTEXT_PING;
    ping.resourceId = info->resourceId;

    exec.command = static_cast<void*>(&ping);
    exec.command_size = sizeof(ping);

    ret = instance->execBuffer(exec, nullptr);
    if (ret)
        return false;

    return true;
}

AddressSpaceStream* createVirtioGpuAddressSpaceStream(HealthMonitor<>* healthMonitor) {
    VirtGpuBlobPtr pipe, blob;
    VirtGpuBlobMappingPtr pipeMapping, blobMapping;
    struct VirtGpuExecBuffer exec = {};
    struct VirtGpuCreateBlob blobCreate = {};
    struct gfxstreamContextCreate contextCreate = {};

    char* blobAddr, *bufferPtr;
    int ret;

    VirtGpuDevice* instance = VirtGpuDevice::getInstance();
    VirtGpuCaps caps = instance->getCaps();

    blobCreate.blobId = 0;
    blobCreate.blobMem = kBlobMemHost3d;
    blobCreate.flags = kBlobFlagMappable;
    blobCreate.size = ALIGN(caps.vulkanCapset.ringSize + caps.vulkanCapset.bufferSize,
                            caps.vulkanCapset.blobAlignment);
    blob = instance->createBlob(blobCreate);
    if (!blob)
        return nullptr;

    // Context creation command
    contextCreate.hdr.opCode = GFXSTREAM_CONTEXT_CREATE;
    contextCreate.resourceId = blob->getResourceHandle();

    exec.command = static_cast<void*>(&contextCreate);
    exec.command_size = sizeof(contextCreate);

    ret = instance->execBuffer(exec, blob);
    if (ret)
        return nullptr;

    // Wait occurs on global timeline -- should we use context specific one?
    ret = blob->wait();
    if (ret)
        return nullptr;

    blobMapping = blob->createMapping();
    if (!blobMapping)
        return nullptr;

    blobAddr = reinterpret_cast<char*>(blobMapping->asRawPtr());

    bufferPtr = blobAddr + sizeof(struct asg_ring_storage);
    struct asg_context context =
        asg_context_create(blobAddr, bufferPtr, caps.vulkanCapset.bufferSize);

    context.ring_config->transfer_mode = 1;
    context.ring_config->host_consumed_pos = 0;
    context.ring_config->guest_write_pos = 0;

    struct address_space_ops ops = {
        .open = virtgpu_address_space_open,
        .close = virtgpu_address_space_close,
        .ping = virtgpu_address_space_ping,
    };

    AddressSpaceStream* res =
            new AddressSpaceStream((address_space_handle_t)(-1), 1, context, 0, 0, ops, healthMonitor);

    res->setMapping(blobMapping);
    res->setResourceId(contextCreate.resourceId);
    return res;
}
