/*
* Copyright (C) 2016 The Android Open Source Project
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

#include "renderControl_enc.h"
#include "qemu_pipe.h"

#if PLATFORM_SDK_VERSION < 26
#include <cutils/log.h>
#else
#include <log/log.h>
#endif
#include <pthread.h>
#include <errno.h>

#ifdef __Fuchsia__
#include <fuchsia/hardware/goldfish/cpp/fidl.h>
#include <lib/fdio/fdio.h>
#include <lib/zx/vmo.h>
static QEMU_PIPE_HANDLE   sProcDevice = 0;
#endif

static QEMU_PIPE_HANDLE   sProcPipe = 0;
static pthread_once_t     sProcPipeOnce = PTHREAD_ONCE_INIT;
// sProcUID is a unique ID per process assigned by the host.
// It is different from getpid().
static uint64_t           sProcUID = 0;

// processPipeInitOnce is used to generate a process unique ID (puid).
// processPipeInitOnce will only be called at most once per process.
// Use it with pthread_once for thread safety.
// The host associates resources with process unique ID (puid) for memory cleanup.
// It will fallback to the default path if the host does not support it.
// Processes are identified by acquiring a per-process 64bit unique ID from the
// host.
#ifdef __Fuchsia__
static void processPipeInitOnce() {
    int fd = ::open(QEMU_PIPE_PATH, O_RDWR);
    if (fd < 0) {
        ALOGE("%s: failed to open " QEMU_PIPE_PATH ": %s",
              __FUNCTION__, strerror(errno));
        return;
    }

    zx::channel channel;
    zx_status_t status = fdio_get_service_handle(
        fd, channel.reset_and_get_address());
    if (status != ZX_OK) {
        ALOGE("%s: failed to get service handle for " QEMU_PIPE_PATH ": %d",
              __FUNCTION__, status);
        close(fd);
        return;
    }

    fuchsia::hardware::goldfish::PipeDeviceSyncPtr device;
    device.Bind(std::move(channel));

    fuchsia::hardware::goldfish::PipeSyncPtr pipe;
    device->OpenPipe(pipe.NewRequest());

    zx_status_t status2 = ZX_OK;
    zx::vmo vmo;
    status = pipe->GetBuffer(&status2, &vmo);
    if (status != ZX_OK || status2 != ZX_OK) {
        ALOGE("%s: failed to get buffer: %d:%d", __FUNCTION__, status, status2);
        return;
    }

    size_t len = strlen("pipe:GLProcessPipe");
    status = vmo.write("pipe:GLProcessPipe", 0, len + 1);
    if (status != ZX_OK) {
        ALOGE("%s: failed write pipe name", __FUNCTION__);
        return;
    }
    uint64_t actual;
    status = pipe->Write(len + 1, 0, &status2, &actual);
    if (status != ZX_OK || status2 != ZX_OK) {
        ALOGD("%s: connecting to pipe service failed: %d:%d", __FUNCTION__,
              status, status2);
        return;
    }

    // Send a confirmation int to the host and get per-process unique ID back
    int32_t confirmInt = 100;
    status = vmo.write(&confirmInt, 0, sizeof(confirmInt));
    if (status != ZX_OK) {
        ALOGE("%s: failed write confirm int", __FUNCTION__);
        return;
    }
    status = pipe->Call(sizeof(confirmInt), 0, sizeof(sProcUID), 0, &status2, &actual);
    if (status != ZX_OK || status2 != ZX_OK) {
        ALOGD("%s: failed to get per-process ID: %d:%d", __FUNCTION__,
              status, status2);
        return;
    }
    status = vmo.read(&sProcUID, 0, sizeof(sProcUID));
    if (status != ZX_OK) {
        ALOGE("%s: failed read per-process ID: %d", __FUNCTION__, status);
        return;
    }
    sProcDevice = device.Unbind().TakeChannel().release();
    sProcPipe = pipe.Unbind().TakeChannel().release();
}
#else
static void processPipeInitOnce() {
    sProcPipe = qemu_pipe_open("GLProcessPipe");
    if (!qemu_pipe_valid(sProcPipe)) {
        sProcPipe = 0;
        ALOGW("Process pipe failed");
        return;
    }
    // Send a confirmation int to the host
    int32_t confirmInt = 100;
    ssize_t stat = 0;
    do {
        stat =
            qemu_pipe_write(sProcPipe, (const char*)&confirmInt,
                sizeof(confirmInt));
    } while (stat < 0 && errno == EINTR);

    if (stat != sizeof(confirmInt)) { // failed
        qemu_pipe_close(sProcPipe);
        sProcPipe = 0;
        ALOGW("Process pipe failed");
        return;
    }

    // Ask the host for per-process unique ID
    do {
        stat =
            qemu_pipe_read(sProcPipe, (char*)&sProcUID,
                sizeof(sProcUID));
    } while (stat < 0 && (errno == EINTR || errno == EAGAIN));

    if (stat != sizeof(sProcUID)) {
        qemu_pipe_close(sProcPipe);
        sProcPipe = 0;
        sProcUID = 0;
        ALOGW("Process pipe failed");
        return;
    }
}
#endif

bool processPipeInit(renderControl_encoder_context_t *rcEnc) {
    pthread_once(&sProcPipeOnce, processPipeInitOnce);
    if (!sProcPipe) return false;
    rcEnc->rcSetPuid(rcEnc, sProcUID);
    return true;
}
