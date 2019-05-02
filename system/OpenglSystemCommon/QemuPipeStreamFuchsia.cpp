/*
* Copyright (C) 2019 The Android Open Source Project
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
#include "QemuPipeStream.h"

#include <cutils/log.h>
#include <errno.h>
#include <lib/fdio/fdio.h>
#include <lib/zx/channel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/process.h>

#include <utility>

QemuPipeStream::QemuPipeStream(size_t bufSize) :
    IOStream(bufSize),
    m_sock(-1),
    m_bufsize(bufSize),
    m_buf(nullptr)
{
}

QemuPipeStream::QemuPipeStream(QEMU_PIPE_HANDLE sock, size_t bufSize) :
    IOStream(bufSize),
    m_sock(sock),
    m_bufsize(bufSize),
    m_buf(nullptr)
{
}

QemuPipeStream::~QemuPipeStream()
{
    if (m_device.is_bound()) {
        flush();
    }
    if (m_buf) {
        zx_status_t status = zx_vmar_unmap(zx_vmar_root_self(),
                                           reinterpret_cast<zx_vaddr_t>(m_buf),
                                           m_bufsize);
        if (status != ZX_OK) {
            ALOGE("zx_vmar_unmap failed: %d\n", status);
            abort();
        }
    }
}

int QemuPipeStream::connect(void)
{
    int fd = TEMP_FAILURE_RETRY(open(QEMU_PIPE_PATH, O_RDWR));
    if (fd < 0) {
        ALOGE("%s: failed to open " QEMU_PIPE_PATH ": %s",
              __FUNCTION__, strerror(errno));
        return -1;
    }

    zx::channel channel;
    zx_status_t status = fdio_get_service_handle(
        fd, channel.reset_and_get_address());
    if (status != ZX_OK) {
        ALOGE("%s: failed to get service handle for " QEMU_PIPE_PATH ": %d",
              __FUNCTION__, status);
        close(fd);
        return -1;
    }
    m_device.Bind(std::move(channel));

    zx::event event;
    status = zx::event::create(0, &event);
    if (status != ZX_OK) {
        ALOGE("%s: failed to create event: %d", __FUNCTION__, status);
        return -1;
    }
    zx::event event_copy;
    status = event.duplicate(ZX_RIGHT_SAME_RIGHTS, &event_copy);
    if (status != ZX_OK) {
        ALOGE("%s: failed to duplicate event: %d", __FUNCTION__, status);
        return -1;
    }

    status = m_device->SetEvent(std::move(event_copy));
    if (status != ZX_OK) {
        ALOGE("%s: failed to set event: %d:%d", __FUNCTION__, status);
        return -1;
    }

    zx_status_t status2 = ZX_OK;
    zx::vmo vmo;
    status = m_device->GetBuffer(&status2, &vmo);
    if (status != ZX_OK || status2 != ZX_OK) {
        ALOGE("%s: failed to get buffer: %d:%d", __FUNCTION__, status, status2);
        return -1;
    }

    size_t len = strlen("pipe:opengles");
    status = vmo.write("pipe:opengles", 0, len + 1);
    if (status != ZX_OK) {
        ALOGE("%s: failed write pipe name", __FUNCTION__);
        return -1;
    }

    uint64_t actual;
    status = m_device->Write(len + 1, 0, &status2, &actual);
    if (status != ZX_OK || status2 != ZX_OK) {
        ALOGD("%s: connecting to pipe service failed: %d:%d", __FUNCTION__,
              status, status2);
        return -1;
    }

    m_event = std::move(event);
    m_vmo = std::move(vmo);
    return 0;
}

void *QemuPipeStream::allocBuffer(size_t minSize)
{
    zx_status_t status;
    if (m_buf) {
        if (minSize <= m_bufsize) {
            return m_buf;
        }
        status = zx_vmar_unmap(zx_vmar_root_self(),
                               reinterpret_cast<zx_vaddr_t>(m_buf),
                               m_bufsize);
        if (status != ZX_OK) {
          ALOGE("zx_vmar_unmap failed: %d\n", status);
          abort();
        }
        m_buf = nullptr;
    }

    size_t allocSize = m_bufsize < minSize ? minSize : m_bufsize;

    zx_status_t status2 = ZX_OK;
    status = m_device->SetBufferSize(allocSize, &status2);
    if (status != ZX_OK || status2 != ZX_OK) {
        ALOGE("%s: failed to get buffer: %d:%d", __FUNCTION__, status, status2);
        return nullptr;
    }

    zx::vmo vmo;
    status = m_device->GetBuffer(&status2, &vmo);
    if (status != ZX_OK || status2 != ZX_OK) {
        ALOGE("%s: failed to get buffer: %d:%d", __FUNCTION__, status, status2);
        return nullptr;
    }

    zx_vaddr_t mapped_addr;
    status = zx_vmar_map(zx_vmar_root_self(),
                         ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                         0, vmo.get(), 0, allocSize, &mapped_addr);
    if (status != ZX_OK) {
        ALOGE("%s: failed to map buffer: %d:%d", __FUNCTION__, status);
        return nullptr;
    }

    m_buf = reinterpret_cast<unsigned char*>(mapped_addr);
    m_bufsize = allocSize;
    m_vmo = std::move(vmo);
    return m_buf;
}

int QemuPipeStream::commitBuffer(size_t size)
{
    if (size == 0) return 0;

    size_t remaining = size;
    while (remaining) {
        zx_status_t status2 = ZX_OK;
        uint64_t actual = 0;
        zx_status_t status = m_device->Write(
            remaining, size - remaining, &status2, &actual);
        if (status != ZX_OK) {
            ALOGD("%s: Failed writing to pipe: %d", __FUNCTION__, status);
            return -1;
        }
        if (actual) {
            remaining -= actual;
            continue;
        }
        if (status2 != ZX_ERR_SHOULD_WAIT) {
            ALOGD("%s: Error writing to pipe: %d", __FUNCTION__, status2);
            return -1;
        }
        zx_signals_t observed = ZX_SIGNAL_NONE;
        status = m_event.wait_one(
            fuchsia::hardware::goldfish::pipe::SIGNAL_WRITABLE |
            fuchsia::hardware::goldfish::pipe::SIGNAL_HANGUP,
            zx::time::infinite(), &observed);
        if (status != ZX_OK) {
            ALOGD("%s: wait_one failed: %d", __FUNCTION__, status);
            return -1;
        }
        if (observed & fuchsia::hardware::goldfish::pipe::SIGNAL_HANGUP) {
            ALOGD("%s: Remote end hungup", __FUNCTION__);
            return -1;
        }
    }

    return 0;
}

int QemuPipeStream::writeFully(const void *buf, size_t len)
{
    ALOGE("%s: unsupported", __FUNCTION__);
    abort();
    return -1;
}

QEMU_PIPE_HANDLE QemuPipeStream::getSocket() const {
    return m_sock;
}

const unsigned char *QemuPipeStream::readFully(void *buf, size_t len)
{
    if (!m_device.is_bound()) return nullptr;

    if (!buf) {
        if (len > 0) {
            ALOGE("QemuPipeStream::readFully failed, buf=NULL, len %zu, lethal"
                    " error, exiting.", len);
            abort();
        }
        return nullptr;
    }

    size_t remaining = len;
    while (remaining) {
        size_t readSize = m_bufsize < remaining ? m_bufsize : remaining;
        zx_status_t status2 = ZX_OK;
        uint64_t actual = 0;
        zx_status_t status = m_device->Read(readSize, 0, &status2, &actual);
        if (status != ZX_OK) {
            ALOGD("%s: Failed reading from pipe: %d", __FUNCTION__, status);
            return nullptr;
        }
        if (actual) {
            m_vmo.read(static_cast<char *>(buf) + (len - remaining), 0, actual);
            remaining -= actual;
            continue;
        }
        if (status2 != ZX_ERR_SHOULD_WAIT) {
            ALOGD("%s: Error reading from pipe: %d", __FUNCTION__, status2);
            return nullptr;
        }
        zx_signals_t observed = ZX_SIGNAL_NONE;
        status = m_event.wait_one(
            fuchsia::hardware::goldfish::pipe::SIGNAL_READABLE |
            fuchsia::hardware::goldfish::pipe::SIGNAL_HANGUP,
            zx::time::infinite(), &observed);
        if (status != ZX_OK) {
            ALOGD("%s: wait_one failed: %d", __FUNCTION__, status);
            return nullptr;
        }
        if (observed & fuchsia::hardware::goldfish::pipe::SIGNAL_HANGUP) {
            ALOGD("%s: Remote end hungup", __FUNCTION__);
            return nullptr;
        }
    }

    return static_cast<const unsigned char *>(buf);
}

const unsigned char *QemuPipeStream::read(void *buf, size_t *inout_len)
{
    ALOGE("%s: unsupported", __FUNCTION__);
    abort();
    return nullptr;
}

int QemuPipeStream::recv(void *buf, size_t len)
{
    ALOGE("%s: unsupported", __FUNCTION__);
    abort();
    return -1;
}
