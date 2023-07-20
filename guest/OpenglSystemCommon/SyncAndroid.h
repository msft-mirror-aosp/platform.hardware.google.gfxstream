// Copyright 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either expresso or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "Sync.h"

#if defined(__ANDROID__)
#include <sync/sync.h>
#endif
#include <unistd.h>

namespace gfxstream {

class SyncHelperAndroid : public SyncHelper {
   public:
    SyncHelperAndroid() = default;

    int wait(int syncFd, int timeoutMilliseconds) override {
#if defined(__ANDROID__)
        return sync_wait(syncFd, timeoutMilliseconds);
#else
        return -1;
#endif
    }

    int dup(int syncFd) override {
        return ::dup(syncFd);
    }

    int close(int syncFd) override {
        return ::close(syncFd);
    }
};

}  // namespace gfxstream
