#if PLATFORM_SDK_VERSION < 26
#include <cutils/log.h>
#else
#include <log/log.h>
#endif

#include "goldfish_address_space.h"

#ifdef HOST_BUILD
GoldfishAddressSpaceBlock::GoldfishAddressSpaceBlock() : m_guest_ptr(NULL) {}
GoldfishAddressSpaceBlock::~GoldfishAddressSpaceBlock() {}

bool GoldfishAddressSpaceBlock::allocate(GoldfishAddressSpaceBlockProvider *provider, size_t size)
{
    return true;
}

uint64_t GoldfishAddressSpaceBlock::physAddr() const
{
    return 42;  // some random number, not used
}

uint64_t GoldfishAddressSpaceBlock::hostAddr() const
{
    return 42;  // some random number, not used
}

void *GoldfishAddressSpaceBlock::mmap(uint64_t opaque)
{
    m_guest_ptr = reinterpret_cast<void *>(opaque);
    return m_guest_ptr;
}

void *GoldfishAddressSpaceBlock::guestPtr() const
{
    return m_guest_ptr;
}

void GoldfishAddressSpaceBlock::destroy()
{
    m_guest_ptr = NULL;
}

void GoldfishAddressSpaceBlock::replace(GoldfishAddressSpaceBlock *other)
{
    if (other) {
        this->m_guest_ptr = other->m_guest_ptr;
        other->m_guest_ptr = NULL;
    } else {
        this->m_guest_ptr = NULL;
    }
}
#elif __Fuchsia__
#include <fcntl.h>
#include <fuchsia/hardware/goldfish/c/fidl.h>
#include <lib/fzl/fdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>

GoldfishAddressSpaceBlockProvider::GoldfishAddressSpaceBlockProvider()
    : m_fd(::open(GOLDFISH_ADDRESS_SPACE_DEVICE_NAME, O_RDWR)) {}

GoldfishAddressSpaceBlockProvider::~GoldfishAddressSpaceBlockProvider()
{
    ::close(m_fd);
}

GoldfishAddressSpaceBlock::GoldfishAddressSpaceBlock()
    : m_vmo(ZX_HANDLE_INVALID)
    , m_mmaped_ptr(NULL)
    , m_phys_addr(0)
    , m_host_addr(0)
    , m_offset(0)
    , m_size(0)
    , m_fd(-1) {}

GoldfishAddressSpaceBlock::~GoldfishAddressSpaceBlock()
{
    destroy();
}

GoldfishAddressSpaceBlock &GoldfishAddressSpaceBlock::operator=(const GoldfishAddressSpaceBlock &rhs)
{
    m_vmo = rhs.m_vmo;
    m_mmaped_ptr = rhs.m_mmaped_ptr;
    m_phys_addr = rhs.m_phys_addr;
    m_host_addr = rhs.m_host_addr;
    m_offset = rhs.m_offset;
    m_size = rhs.m_size;
    m_fd = rhs.m_fd;

    return *this;
}

bool GoldfishAddressSpaceBlock::allocate(GoldfishAddressSpaceBlockProvider *provider, size_t size)
{
    ALOGD("%s: Ask for block of size 0x%llx\n", __func__,
         (unsigned long long)size);

    destroy();

    if (!provider->is_opened()) {
        return false;
    }

    {
        fzl::FdioCaller caller{fbl::unique_fd(provider->m_fd)};

        int32_t res = ZX_OK;
        zx_status_t status =
            fuchsia_hardware_goldfish_DeviceAllocateVmo(caller.borrow_channel(),
                                                        size, &res, &m_vmo);
        if (status != ZX_OK || res != ZX_OK) {
            ALOGE("%s: allocate vmo failed: %d:%d", __func__, status, res);
            provider->m_fd = caller.release().release();
            return false;
        }

        zx_handle_t vmo_out;
        status = zx_handle_duplicate(m_vmo, ZX_DEFAULT_VMO_RIGHTS, &vmo_out);
        if (status != ZX_OK) {
            ALOGE("%s: vmo dup failed: %d:%d", __func__, status);
            provider->m_fd = caller.release().release();
            return false;
        }

        status = fuchsia_hardware_goldfish_DeviceGetPhysicalAddress(
            caller.borrow_channel(),
            vmo_out, &res, &m_phys_addr);
        provider->m_fd = caller.release().release();

        if (status != ZX_OK || res != ZX_OK) {
            ALOGE("%s: pin vmo failed: %d:%d", __func__, status, res);
            return false;
        }
    }

    m_offset = 0;
    m_size = size;

    ALOGD("%s: allocate returned offset 0x%llx size 0x%llx\n", __func__,
          (unsigned long long)m_offset,
          (unsigned long long)m_size);

    m_fd = provider->m_fd;
    return true;
}

uint64_t GoldfishAddressSpaceBlock::physAddr() const
{
    return m_phys_addr;
}

uint64_t GoldfishAddressSpaceBlock::hostAddr() const
{
    return m_host_addr;
}

void *GoldfishAddressSpaceBlock::mmap(uint64_t host_addr)
{
    if (m_size == 0) {
        ALOGE("%s: called with zero size\n", __func__);
        return NULL;
    }
    if (m_mmaped_ptr) {
        ALOGE("'mmap' called for an already mmaped address block");
        ::abort();
    }

    zx_vaddr_t ptr = 0;
    zx_status_t status = zx_vmar_map(zx_vmar_root_self(),
                                     ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                                     0, m_vmo,
                                     m_offset,
                                     m_size,
                                     &ptr);
    if (status != ZX_OK) {
        ALOGE("%s: host memory map failed with size 0x%llx "
              "off 0x%llx status %d\n",
              __func__,
              (unsigned long long)m_size,
              (unsigned long long)m_offset, status);
        return NULL;
    } else {
        m_mmaped_ptr = (void*)ptr;
        m_host_addr = host_addr;
        return guestPtr();
    }
}

void *GoldfishAddressSpaceBlock::guestPtr() const
{
    return reinterpret_cast<char *>(m_mmaped_ptr) + (m_host_addr & (PAGE_SIZE - 1));
}

void GoldfishAddressSpaceBlock::destroy()
{
    if (m_mmaped_ptr && m_size) {
        zx_vmar_unmap(zx_vmar_root_self(),
                      (zx_vaddr_t)m_mmaped_ptr,
                      m_size);
        m_mmaped_ptr = NULL;
    }

    if (m_size) {
        zx_handle_close(m_vmo);
        m_vmo = ZX_HANDLE_INVALID;
        m_phys_addr = 0;
        m_host_addr = 0;
        m_offset = 0;
        m_size = 0;
    }
}

void GoldfishAddressSpaceBlock::replace(GoldfishAddressSpaceBlock *other)
{
    destroy();

    if (other) {
        *this = *other;
        *other = GoldfishAddressSpaceBlock();
    }
}

bool GoldfishAddressSpaceBlockProvider::is_opened()
{
    return m_fd >= 0;
}
#else
#include <linux/types.h>
#include <linux/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <errno.h>

struct goldfish_address_space_allocate_block {
    __u64 size;
    __u64 offset;
    __u64 phys_addr;
};

#define GOLDFISH_ADDRESS_SPACE_IOCTL_MAGIC		'G'
#define GOLDFISH_ADDRESS_SPACE_IOCTL_OP(OP, T)		_IOWR(GOLDFISH_ADDRESS_SPACE_IOCTL_MAGIC, OP, T)
#define GOLDFISH_ADDRESS_SPACE_IOCTL_ALLOCATE_BLOCK	GOLDFISH_ADDRESS_SPACE_IOCTL_OP(10, struct goldfish_address_space_allocate_block)
#define GOLDFISH_ADDRESS_SPACE_IOCTL_DEALLOCATE_BLOCK	GOLDFISH_ADDRESS_SPACE_IOCTL_OP(11, __u64)

const char GOLDFISH_ADDRESS_SPACE_DEVICE_NAME[] = "/dev/goldfish_address_space";

GoldfishAddressSpaceBlockProvider::GoldfishAddressSpaceBlockProvider()
    : m_fd(::open(GOLDFISH_ADDRESS_SPACE_DEVICE_NAME, O_RDWR)) {}

GoldfishAddressSpaceBlockProvider::~GoldfishAddressSpaceBlockProvider()
{
    ::close(m_fd);
}

GoldfishAddressSpaceBlock::GoldfishAddressSpaceBlock()
    : m_mmaped_ptr(NULL)
    , m_phys_addr(0)
    , m_host_addr(0)
    , m_offset(0)
    , m_size(0)
    , m_fd(-1) {}

GoldfishAddressSpaceBlock::~GoldfishAddressSpaceBlock()
{
    destroy();
}

GoldfishAddressSpaceBlock &GoldfishAddressSpaceBlock::operator=(const GoldfishAddressSpaceBlock &rhs)
{
    m_mmaped_ptr = rhs.m_mmaped_ptr;
    m_phys_addr = rhs.m_phys_addr;
    m_host_addr = rhs.m_host_addr;
    m_offset = rhs.m_offset;
    m_size = rhs.m_size;
    m_fd = rhs.m_fd;

    return *this;
}

bool GoldfishAddressSpaceBlock::allocate(GoldfishAddressSpaceBlockProvider *provider, size_t size)
{

    ALOGD("%s: Ask for block of size 0x%llx\n", __func__,
         (unsigned long long)size);

    destroy();

    if (!provider->is_opened()) {
        return false;
    }

    struct goldfish_address_space_allocate_block request;
    long res;

    request.size = size;
    res = ::ioctl(provider->m_fd, GOLDFISH_ADDRESS_SPACE_IOCTL_ALLOCATE_BLOCK, &request);
    if (res) {
        return false;
    } else {
        m_phys_addr = request.phys_addr;

        m_offset = request.offset;
        m_size = request.size;

        ALOGD("%s: ioctl allocate returned offset 0x%llx size 0x%llx\n", __func__,
                (unsigned long long)m_offset,
                (unsigned long long)m_size);

        m_fd = provider->m_fd;
        return true;
    }
}

uint64_t GoldfishAddressSpaceBlock::physAddr() const
{
    return m_phys_addr;
}

uint64_t GoldfishAddressSpaceBlock::hostAddr() const
{
    return m_host_addr;
}

void *GoldfishAddressSpaceBlock::mmap(uint64_t host_addr)
{
    if (m_size == 0) {
        ALOGE("%s: called with zero size\n", __func__);
        return NULL;
    }
    if (m_mmaped_ptr) {
        ALOGE("'mmap' called for an already mmaped address block");
        ::abort();
    }

    void *result = ::mmap64(NULL, m_size, PROT_WRITE, MAP_SHARED, m_fd, m_offset);
    if (result == MAP_FAILED) {
        ALOGE("%s: host memory map failed with size 0x%llx "
              "off 0x%llx errno %d\n",
              __func__,
              (unsigned long long)m_size,
              (unsigned long long)m_offset, errno);
        return NULL;
    } else {
        m_mmaped_ptr = result;
        m_host_addr = host_addr;
        return guestPtr();
    }
}

void *GoldfishAddressSpaceBlock::guestPtr() const
{
    return reinterpret_cast<char *>(m_mmaped_ptr) + (m_host_addr & (PAGE_SIZE - 1));
}

void GoldfishAddressSpaceBlock::destroy()
{
    if (m_mmaped_ptr && m_size) {
        ::munmap(m_mmaped_ptr, m_size);
        m_mmaped_ptr = NULL;
    }

    if (m_size) {
        ::ioctl(m_fd, GOLDFISH_ADDRESS_SPACE_IOCTL_DEALLOCATE_BLOCK, &m_offset);
        m_phys_addr = 0;
        m_host_addr = 0;
        m_offset = 0;
        m_size = 0;
    }
}

void GoldfishAddressSpaceBlock::replace(GoldfishAddressSpaceBlock *other)
{
    destroy();

    if (other) {
        *this = *other;
        *other = GoldfishAddressSpaceBlock();
    }
}

bool GoldfishAddressSpaceBlockProvider::is_opened()
{
    return m_fd >= 0;
}
#endif
