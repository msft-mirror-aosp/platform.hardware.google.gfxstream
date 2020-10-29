/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <atomic>
#include <errno.h>
#include <log/log.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/vm_sockets.h>
#include <qemu_pipe_bp.h>

namespace {
enum class VsockPort {
    Data = 5555,
    Ping = 5556,
};

std::atomic<bool> gVsockAvailable = false;

int open_verbose_path(const char* name, const int flags) {
    const int fd = QEMU_PIPE_RETRY(open(name, flags));
    if (fd < 0) {
        ALOGE("%s:%d: Could not open '%s': %s",
              __func__, __LINE__, name, strerror(errno));
    }
    return fd;
}

int open_verbose_vsock(const VsockPort port, const int flags, const bool logConnectError) {
    const int fd = QEMU_PIPE_RETRY(socket(AF_VSOCK, SOCK_STREAM, 0));
    if (fd < 0) {
        ALOGE("%s:%d socket(AF_VSOCK, SOCK_STREAM) failed with '%s' (%d)",
              __func__, __LINE__, strerror(errno), errno);
        return -1;
    }

    struct sockaddr_vm sa;
    memset(&sa, 0, sizeof(sa));
    sa.svm_family = AF_VSOCK;
    sa.svm_port = static_cast<int>(port);
    sa.svm_cid = VMADDR_CID_HOST;

    int r;

    r = QEMU_PIPE_RETRY(connect(fd,
                                reinterpret_cast<const struct sockaddr*>(&sa),
                                sizeof(sa)));
    if (r < 0) {
        if (logConnectError) {
            ALOGE("%s:%d connect(fd=%d, port=%d) failed with '%s' (%d)",
                  __func__, __LINE__, fd, sa.svm_port, strerror(errno), errno);
        }

        close(fd);
        return -1;
    }

    if (flags) {
        const int oldFlags = QEMU_PIPE_RETRY(fcntl(fd, F_GETFL, 0));
        if (oldFlags < 0) {
            ALOGE("%s:%d fcntl(fd=%d, F_GETFL) failed with '%s' (%d)",
                  __func__, __LINE__, fd, strerror(errno), errno);
            close(fd);
            return -1;
        }

        const int newFlags = oldFlags | flags;

        r = QEMU_PIPE_RETRY(fcntl(fd, F_SETFL, newFlags));
        if (r < 0) {
            ALOGE("%s:%d fcntl(fd=%d, F_SETFL, flags=0x%X) failed with '%s' (%d)",
                  __func__, __LINE__, fd, newFlags, strerror(errno), errno);
            close(fd);
            return -1;
        }
    }

    return fd;
}

int open_verbose(const char *pipeName, const int flags) {
    // "opengles" crashes the kernel, see b/171252755.
    // It should be ok to remove once the crash is fixed.
    if (strcmp(pipeName, "opengles")) {
        const int fd = open_verbose_vsock(VsockPort::Data, flags, true);
        if (fd >= 0) {
            gVsockAvailable = true;
            return fd;
        }
    }

    return open_verbose_path("/dev/goldfish_pipe", flags);
}

void vsock_ping() {
    const int fd = open_verbose_vsock(VsockPort::Ping, 0, false);
    if (fd >= 0) {
        ALOGE("%s:%d open_verbose_vsock(kVsockPingPort) is expected to fail, "
              "but it succeeded, fd=%d", __func__, __LINE__, fd);
        close(fd);
    }
}

}  // namespace

extern "C" {

int qemu_pipe_open_ns(const char* ns, const char* pipeName, int flags) {
    if (pipeName == NULL || pipeName[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    const int fd = open_verbose(pipeName, flags);
    if (fd < 0) {
        return fd;
    }

    char buf[256];
    int bufLen;
    if (ns) {
        bufLen = snprintf(buf, sizeof(buf), "pipe:%s:%s", ns, pipeName);
    } else {
        bufLen = snprintf(buf, sizeof(buf), "pipe:%s", pipeName);
    }

    if (qemu_pipe_write_fully(fd, buf, bufLen + 1)) {
        ALOGE("%s:%d: Could not connect to the '%s' service: %s",
              __func__, __LINE__, buf, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

int qemu_pipe_open(const char* pipeName) {
    return qemu_pipe_open_ns(NULL, pipeName, O_RDWR | O_NONBLOCK);
}

void qemu_pipe_close(int pipe) {
    close(pipe);
}

int qemu_pipe_read(int pipe, void* buffer, int size) {
    return read(pipe, buffer, size);
}

int qemu_pipe_write(int pipe, const void* buffer, int size) {
    return write(pipe, buffer, size);
}

int qemu_pipe_try_again(int ret) {
    if (ret >= 0) {
        return 0;
    }

    switch (errno) {
    case EAGAIN:
        if (gVsockAvailable) {
            vsock_ping();
            errno = EAGAIN;
        }
        return 1;

    case EINTR:
        return 1;

    default:
        return 0;
    }
}

void qemu_pipe_print_error(int pipe) {
    ALOGE("pipe error: fd %d errno %d", pipe, errno);
}

}  // extern "C"
