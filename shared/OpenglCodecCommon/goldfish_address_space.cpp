#include <log/log.h>
#include "goldfish_address_space.h"

#ifdef GOLDFISH_OPENGL_BUILD_FOR_HOST
GoldfishAddressSpaceBlock::GoldfishAddressSpaceBlock() : m_guest_ptr(NULL) {}

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
        return NULL;
    }
    if (m_mmaped_ptr) {
        ALOGE("'mmap' called for an already mmaped address block");
        ::abort();
    }

    void *result = ::mmap(NULL, m_size, PROT_WRITE, MAP_SHARED, m_fd, m_offset);
    if (result == MAP_FAILED) {
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
