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

#ifndef ANDROID_INCLUDE_HARDWARE_GOLDFISH_ADDRESS_SPACE_H
#define ANDROID_INCLUDE_HARDWARE_GOLDFISH_ADDRESS_SPACE_H

#include <inttypes.h>
#include <stddef.h>

#ifdef __Fuchsia__
#include <fuchsia/hardware/goldfish/address/space/cpp/fidl.h>
#endif

class GoldfishAddressSpaceBlock;
class GoldfishAddressSpaceHostMemoryAllocator;

#ifdef HOST_BUILD

namespace android {

class HostAddressSpaceDevice;

} // namespace android

#endif

#if defined(__Fuchsia__)
    typedef void* address_space_handle_t;
#elif defined(HOST_BUILD)
    typedef uint32_t address_space_handle_t;
#else
    typedef int address_space_handle_t;
#endif

class GoldfishAddressSpaceBlockProvider {
public:
    static const uint64_t SUBDEVICE_TYPE_NO_SUBDEVICE_ID = -1;
    static const uint64_t SUBDEVICE_TYPE_HOST_MEMORY_ALLOCATOR_ID = 5;
    GoldfishAddressSpaceBlockProvider(uint64_t subdevice);
    ~GoldfishAddressSpaceBlockProvider();

private:
    GoldfishAddressSpaceBlockProvider(const GoldfishAddressSpaceBlockProvider &rhs);
    GoldfishAddressSpaceBlockProvider &operator=(const GoldfishAddressSpaceBlockProvider &rhs);

    bool is_opened() const;
    void close();
    address_space_handle_t release();
    static void closeHandle(address_space_handle_t handle);

#ifdef __Fuchsia__
    fuchsia::hardware::goldfish::address::space::DeviceSyncPtr m_device;
#else // __Fuchsia__
    address_space_handle_t m_handle;
#endif // !__Fuchsia__

    friend class GoldfishAddressSpaceBlock;
    friend class GoldfishAddressSpaceHostMemoryAllocator;
};

class GoldfishAddressSpaceBlock {
public:
    GoldfishAddressSpaceBlock();
    ~GoldfishAddressSpaceBlock();

    bool allocate(GoldfishAddressSpaceBlockProvider *provider, size_t size);
    uint64_t physAddr() const;
    uint64_t hostAddr() const;
    uint64_t offset() const { return m_offset; }
    size_t size() const { return m_size; }
    void *mmap(uint64_t opaque);
    void *guestPtr() const;
    void replace(GoldfishAddressSpaceBlock *other);
    void release();
    static int memoryMap(void *addr, size_t len, address_space_handle_t fd, uint64_t off, void** dst);
    static void memoryUnmap(void *ptr, size_t size);

private:
    void destroy();
    GoldfishAddressSpaceBlock &operator=(const GoldfishAddressSpaceBlock &);

#ifdef __Fuchsia__
    fuchsia::hardware::goldfish::address::space::DeviceSyncPtr* m_device;
    uint32_t  m_vmo;
#else // __Fuchsia__
    address_space_handle_t m_handle;
#endif // !__Fuchsia__

    void     *m_mmaped_ptr;
    uint64_t  m_phys_addr;
    uint64_t  m_host_addr;
    uint64_t  m_offset;
    uint64_t  m_size;
};

class GoldfishAddressSpaceHostMemoryAllocator {
public:
    GoldfishAddressSpaceHostMemoryAllocator();

    long hostMalloc(GoldfishAddressSpaceBlock *block, size_t size);
    void hostFree(GoldfishAddressSpaceBlock *block);

    bool is_opened() const;
    address_space_handle_t release() { return m_provider.release(); }
    static void closeHandle(address_space_handle_t handle) { GoldfishAddressSpaceBlockProvider::closeHandle(handle); }

private:
    GoldfishAddressSpaceBlockProvider m_provider;
};

#endif  // #ifndef ANDROID_INCLUDE_HARDWARE_GOLDFISH_ADDRESS_SPACE_H
