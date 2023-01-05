/*
* Copyright (C) 2021 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#ifndef __COMMAND_BUFFER_STAGING_STREAM_H
#define __COMMAND_BUFFER_STAGING_STREAM_H

#include "IOStream.h"
#include <functional>

class CommandBufferStagingStream : public IOStream {
public:
 // host will write kSyncDataReadComplete to the sync bytes to indicate memory is no longer being
 // used by host. This is only used with custom allocators. The sync bytes are used to ensure that,
 // during reallocations the guest does not free memory being read by the host. The guest ensures
 // that the sync bytes are marked as read complete before releasing the memory.
 static constexpr size_t kSyncDataSize = 8;
 // indicates read is complete
 static constexpr uint32_t kSyncDataReadComplete = 0X0;
 // indicates read is pending
 static constexpr uint32_t kSyncDataReadPending = 0X1;

 // allocator
 // param size to allocate
 // return pointer to allocated memory
 using Alloc = std::function<void*(size_t)>;
 // free function
 // param pointer to free
 using Free = std::function<void(void*)>;
 // constructor
 // \param allocFn is the allocation function provided. Default allocation function used if nullptr
 // \param freeFn is the free function provided. Default free function used if nullptr
 // freeFn must be provided if allocFn is provided and vice versa
 explicit CommandBufferStagingStream(Alloc&& allocFn = nullptr, Free&& freeFn = nullptr);
 ~CommandBufferStagingStream();

 virtual size_t idealAllocSize(size_t len);
 virtual void* allocBuffer(size_t minSize);
 virtual int commitBuffer(size_t size);
 virtual const unsigned char* readFully(void* buf, size_t len);
 virtual const unsigned char* read(void* buf, size_t* inout_len);
 virtual int writeFully(const void* buf, size_t len);
 virtual const unsigned char* commitBufferAndReadFully(size_t size, void* buf, size_t len);

 void getWritten(unsigned char** bufOut, size_t* sizeOut);
 void reset();

 // marks the command buffer stream as flushing. The owner of CommandBufferStagingStream
 // should call markFlushing after finishing writing to the stream.
 // This will mark the sync data to kSyncDataReadPending. This is only applicable when
 // using custom allocators. markFlushing will be a no-op if called
 // when not using custom allocators
 void markFlushing();

private:
 // underlying buffer for data
 unsigned char* m_buf;
 // size of portion of m_buf available for data.
 // for custom allocation, this size excludes size of sync data.
 size_t m_size;
 // current write position in m_buf
 uint32_t m_writePos;

 Alloc m_alloc;
 Free m_free;

 // underlying custom alloc. default is null
 Alloc m_customAlloc = nullptr;
 // underlying free alloc. default is null
 Free m_customFree = nullptr;

 // realloc function
 // \param size of memory to be allocated
 // \ param reference size to update with actual size allocated. This size can be < requested size
 // for custom allocation to account for sync data
 using Realloc = std::function<void*(void*, size_t)>;
 Realloc m_realloc;

 // flag tracking use of custom allocation/free
 bool m_usingCustomAlloc = false;

 // calculates actual allocation size for data
 // \param requestedSize is the size requested for allocation
 // \return actual data size allocated for requested size. For
 // custom allocation the data size < requested size to account for sync data word
 size_t getDataAllocationSize(const size_t requestedSize);
};

#endif
