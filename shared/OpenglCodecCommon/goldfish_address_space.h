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

#ifdef HOST_BUILD

namespace android {

class HostAddressSpaceDevice;

} // namespace android

#endif

class GoldfishAddressSpaceBlockProvider {
public:
    GoldfishAddressSpaceBlockProvider();
    ~GoldfishAddressSpaceBlockProvider();

private:
   GoldfishAddressSpaceBlockProvider(const GoldfishAddressSpaceBlockProvider &rhs);
   GoldfishAddressSpaceBlockProvider &operator=(const GoldfishAddressSpaceBlockProvider &rhs);

   bool is_opened();
#ifdef HOST_BUILD
   uint32_t m_handle;
#else // HOST_BUILD
#ifdef __Fuchsia__
   fuchsia::hardware::goldfish::address::space::DeviceSyncPtr m_device;
#else // __Fuchsia__
   int m_fd;
#endif // !__Fuchsia__
#endif // !HOST_BUILD

   friend class GoldfishAddressSpaceBlock;
};

class GoldfishAddressSpaceBlock {
public:
    GoldfishAddressSpaceBlock();
    ~GoldfishAddressSpaceBlock();

    bool allocate(GoldfishAddressSpaceBlockProvider *provider, size_t size);
    uint64_t physAddr() const;
    uint64_t hostAddr() const;
    void *mmap(uint64_t opaque);
    void *guestPtr() const;
    void replace(GoldfishAddressSpaceBlock *x);

private:
    void destroy();
    GoldfishAddressSpaceBlock &operator=(const GoldfishAddressSpaceBlock &);

#ifdef HOST_BUILD
    uint32_t m_handle;
#else // HOST_BUILD

#ifdef __Fuchsia__
    fuchsia::hardware::goldfish::address::space::DeviceSyncPtr* m_device;
    uint32_t  m_vmo;

#else // __Fuchsia__

    int       m_fd;

#endif // !__Fuchsia__
#endif // !HOST_BUILD

    void     *m_mmaped_ptr;
    uint64_t  m_phys_addr;
    uint64_t  m_host_addr;
    uint64_t  m_offset;
    size_t    m_size;
};

#endif
