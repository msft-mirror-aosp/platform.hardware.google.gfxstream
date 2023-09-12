/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "VirtioGpuPipeStream.h"

#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <xf86drm.h>

#include "virtgpu_drm.h"
#include "VirtGpu.h"

// In a virtual machine, there should only be one GPU
#define RENDERNODE_MINOR 128

// Attributes use to allocate our response buffer
// Similar to virgl's fence objects
#define PIPE_BUFFER             0
#define VIRGL_FORMAT_R8_UNORM   64
#define VIRGL_BIND_CUSTOM       (1 << 17)

static const size_t kTransferBufferSize = (1048576);

static const size_t kReadSize = 512 * 1024;
static const size_t kWriteOffset = kReadSize;

VirtioGpuPipeStream::VirtioGpuPipeStream(size_t bufSize) :
    IOStream(bufSize),
    m_virtio_mapped(nullptr),
    m_bufsize(bufSize),
    m_buf(nullptr),
    m_read(0),
    m_readLeft(0),
    m_writtenPos(0) { }

VirtioGpuPipeStream::VirtioGpuPipeStream(size_t bufSize, int fd) :
    IOStream(bufSize),
    m_fd(fd),
    m_virtio_mapped(nullptr),
    m_bufsize(bufSize),
    m_buf(nullptr),
    m_read(0),
    m_readLeft(0),
    m_writtenPos(0) { }

VirtioGpuPipeStream::~VirtioGpuPipeStream()
{
    free(m_buf);
}

bool VirtioGpuPipeStream::valid() {
    return m_device != nullptr;
}

int VirtioGpuPipeStream::getRendernodeFd() {
    if (m_device == nullptr) {
        return -1;
    }
    return m_device->getDeviceHandle();
}

int VirtioGpuPipeStream::connect(const char* serviceName)
{
    if (!m_device) {
        m_device.reset(createPlatformVirtGpuDevice(kCapsetNone, m_fd));
        if (!m_device) {
            ALOGE("Failed to create VirtioGpuPipeStream VirtGpuDevice.");
            return -1;
        }

        m_resource = m_device->createPipeBlob(kTransferBufferSize);
        if (!m_resource) {
            ALOGE("Failed to create VirtioGpuPipeStream resource.");
            return -1;
        }

        m_resourceMapping = m_resource->createMapping();
        if (!m_resourceMapping) {
            ALOGE("Failed to create VirtioGpuPipeStream resource mapping.");
            return -1;
        }

        m_virtio_mapped = m_resourceMapping->asRawPtr();
        if (!m_virtio_mapped) {
            ALOGE("Failed to create VirtioGpuPipeStream resource mapping ptr.");
            return -1;
        }
    }

    wait();

    if (serviceName) {
        writeFully(serviceName, strlen(serviceName) + 1);
    } else {
        static const char kPipeString[] = "pipe:opengles";
        std::string pipeStr(kPipeString);
        writeFully(kPipeString, sizeof(kPipeString));
    }

    return 0;
}

int VirtioGpuPipeStream::openRendernode() {
    int fd = drmOpenRender(RENDERNODE_MINOR);
    if (fd < 0) {
            ERR("%s: failed with fd %d (%s)", __func__, fd, strerror(errno));
        return -1;
    }
    return fd;
}

uint64_t VirtioGpuPipeStream::initProcessPipe() {
    connect("pipe:GLProcessPipe");
    int32_t confirmInt = 100;
    writeFully(&confirmInt, sizeof(confirmInt));
    uint64_t res;
    readFully(&res, sizeof(res));
    return res;
}

void *VirtioGpuPipeStream::allocBuffer(size_t minSize) {
    size_t allocSize = (m_bufsize < minSize ? minSize : m_bufsize);
    if (!m_buf) {
        m_buf = (unsigned char *)malloc(allocSize);
    }
    else if (m_bufsize < allocSize) {
        unsigned char *p = (unsigned char *)realloc(m_buf, allocSize);
        if (p != NULL) {
            m_buf = p;
            m_bufsize = allocSize;
        } else {
            ERR("realloc (%zu) failed\n", allocSize);
            free(m_buf);
            m_buf = NULL;
            m_bufsize = 0;
        }
    }

    return m_buf;
}

int VirtioGpuPipeStream::commitBuffer(size_t size) {
    if (size == 0) return 0;
    return writeFully(m_buf, size);
}

int VirtioGpuPipeStream::writeFully(const void *buf, size_t len)
{
    //DBG(">> VirtioGpuPipeStream::writeFully %d\n", len);
    if (!valid()) return -1;
    if (!buf) {
       if (len>0) {
            // If len is non-zero, buf must not be NULL. Otherwise the pipe would be
            // in a corrupted state, which is lethal for the emulator.
           ERR("VirtioGpuPipeStream::writeFully failed, buf=NULL, len %zu,"
                   " lethal error, exiting", len);
           abort();
       }
       return 0;
    }

    size_t res = len;
    int retval = 0;

    while (res > 0) {
        ssize_t stat = transferToHost((const char *)(buf) + (len - res), res);
        if (stat > 0) {
            res -= stat;
            continue;
        }
        if (stat == 0) { /* EOF */
            ERR("VirtioGpuPipeStream::writeFully failed: premature EOF\n");
            retval = -1;
            break;
        }
        if (errno == EAGAIN) {
            continue;
        }
        retval =  stat;
        ERR("VirtioGpuPipeStream::writeFully failed: %s, lethal error, exiting.\n",
                strerror(errno));
        abort();
    }
    //DBG("<< VirtioGpuPipeStream::writeFully %d\n", len );
    return retval;
}

const unsigned char *VirtioGpuPipeStream::readFully(void *buf, size_t len)
{
    flush();

    if (!valid()) return NULL;
    if (!buf) {
        if (len > 0) {
            // If len is non-zero, buf must not be NULL. Otherwise the pipe would be
            // in a corrupted state, which is lethal for the emulator.
            ERR("VirtioGpuPipeStream::readFully failed, buf=NULL, len %zu, lethal"
                    " error, exiting.", len);
            abort();
        }
    }

    size_t res = len;
    while (res > 0) {
        ssize_t stat = transferFromHost((char *)(buf) + len - res, res);
        if (stat == 0) {
            // client shutdown;
            return NULL;
        } else if (stat < 0) {
            if (errno == EAGAIN) {
                continue;
            } else {
                ERR("VirtioGpuPipeStream::readFully failed (buf %p, len %zu"
                    ", res %zu): %s, lethal error, exiting.", buf, len, res,
                    strerror(errno));
                abort();
            }
        } else {
            res -= stat;
        }
    }
    //DBG("<< VirtioGpuPipeStream::readFully %d\n", len);
    return (const unsigned char *)buf;
}

const unsigned char *VirtioGpuPipeStream::commitBufferAndReadFully(
    size_t writeSize, void *userReadBufPtr, size_t totalReadSize)
{
    return commitBuffer(writeSize) ? nullptr : readFully(userReadBufPtr, totalReadSize);
}

const unsigned char *VirtioGpuPipeStream::read( void *buf, size_t *inout_len)
{
    //DBG(">> VirtioGpuPipeStream::read %d\n", *inout_len);
    if (!valid()) return NULL;
    if (!buf) {
      ERR("VirtioGpuPipeStream::read failed, buf=NULL");
      return NULL;  // do not allow NULL buf in that implementation
    }

    int n = recv(buf, *inout_len);

    if (n > 0) {
        *inout_len = n;
        return (const unsigned char *)buf;
    }

    //DBG("<< VirtioGpuPipeStream::read %d\n", *inout_len);
    return NULL;
}

int VirtioGpuPipeStream::recv(void *buf, size_t len)
{
    if (!valid()) return int(ERR_INVALID_SOCKET);
    char* p = (char *)buf;
    int ret = 0;
    while(len > 0) {
        int res = transferFromHost(p, len);
        if (res > 0) {
            p += res;
            ret += res;
            len -= res;
            continue;
        }
        if (res == 0) { /* EOF */
             break;
        }
        if (errno != EAGAIN) {
            continue;
        }

        /* A real error */
        if (ret == 0)
            ret = -1;
        break;
    }
    return ret;
}

void VirtioGpuPipeStream::wait() {
    int ret = m_resource->wait();
    if (ret) {
        ERR("VirtioGpuPipeStream: DRM_IOCTL_VIRTGPU_WAIT failed with %d (%s)\n", errno, strerror(errno));
    }

    m_writtenPos = 0;
}

ssize_t VirtioGpuPipeStream::transferToHost(const void* buffer, size_t len) {
    size_t todo = len;
    size_t done = 0;
    int ret = EAGAIN;

    unsigned char* virtioPtr = m_virtio_mapped;

    const unsigned char* readPtr = reinterpret_cast<const unsigned char*>(buffer);

    while (done < len) {
        size_t toXfer = todo > kTransferBufferSize ? kTransferBufferSize : todo;

        if (toXfer > (kTransferBufferSize - m_writtenPos)) {
            wait();
        }

        memcpy(virtioPtr + m_writtenPos, readPtr, toXfer);

        ret = m_resource->transferToHost(m_writtenPos, toXfer);
        if (ret) {
            ERR("VirtioGpuPipeStream: failed to transferToHost() with errno %d (%s)\n", errno, strerror(errno));
            return (ssize_t)ret;
        }

        done += toXfer;
        readPtr += toXfer;
		todo -= toXfer;
        m_writtenPos += toXfer;
    }

    return len;
}

ssize_t VirtioGpuPipeStream::transferFromHost(void* buffer, size_t len) {
    size_t todo = len;
    size_t done = 0;
    int ret = EAGAIN;

    const unsigned char* virtioPtr = m_virtio_mapped;
    unsigned char* readPtr = reinterpret_cast<unsigned char*>(buffer);

    if (m_writtenPos) {
        wait();
    }

    while (done < len) {
        size_t toXfer = todo > kTransferBufferSize ? kTransferBufferSize : todo;

        ret = m_resource->transferFromHost(0, toXfer);
        if (ret) {
            ERR("VirtioGpuPipeStream: failed to transferFromHost() with errno %d (%s)\n", errno, strerror(errno));
            return (ssize_t)ret;
        }

        wait();

        memcpy(readPtr, virtioPtr, toXfer);

        done += toXfer;
        readPtr += toXfer;
	    todo -= toXfer;
    }

    return len;
}
