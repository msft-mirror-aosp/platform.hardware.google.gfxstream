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

#include "magma_enc_util.h"

#include <cstring>

namespace magma_enc_util {

size_t size_command_descriptor(magma_command_descriptor* descriptor) {
    uint64_t size = sizeof(magma_command_descriptor);
    size += sizeof(magma_exec_resource) * descriptor->resource_count;
    size += sizeof(magma_exec_command_buffer) * descriptor->command_buffer_count;
    size +=
        sizeof(uint64_t) * (descriptor->wait_semaphore_count + descriptor->signal_semaphore_count);
    return size;
}

void pack_command_descriptor(void* void_ptr, magma_connection_t connection, uint32_t context_id,
                             magma_command_descriptor* descriptor) {
    magma_exec_resource* resources = descriptor->resources;
    magma_exec_command_buffer* command_buffers = descriptor->command_buffers;
    uint64_t* semaphore_ids = descriptor->semaphore_ids;

    magma_command_descriptor desc_copy = *descriptor;
    desc_copy.resources = 0;
    desc_copy.command_buffers = 0;
    desc_copy.semaphore_ids = 0;

    auto ptr = reinterpret_cast<uint8_t*>(void_ptr);

    memcpy(ptr, &desc_copy, sizeof(magma_command_descriptor));
    ptr += sizeof(magma_command_descriptor);

    memcpy(ptr, resources, sizeof(magma_exec_resource) * descriptor->resource_count);
    ptr += sizeof(magma_exec_resource) * descriptor->resource_count;

    memcpy(ptr, command_buffers,
           sizeof(magma_exec_command_buffer) * descriptor->command_buffer_count);
    ptr += sizeof(magma_exec_command_buffer) * descriptor->command_buffer_count;

    memcpy(
        ptr, semaphore_ids,
        sizeof(uint64_t) * (descriptor->wait_semaphore_count + descriptor->signal_semaphore_count));
}

}  // namespace magma_enc_util
