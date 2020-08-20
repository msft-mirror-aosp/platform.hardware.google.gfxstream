/*
* Copyright (C) 2020 The Android Open Source Project
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
#ifndef _STATE_TRACKING_SUPPORT_H_
#define _STATE_TRACKING_SUPPORT_H_

#include "android/base/containers/HybridComponentManager.h"

template <bool initialIsTrue>
class PredicateMap {
public:
    static const uint64_t kBitsPerEntry = 64;
    void add(uint32_t objId) {
        static const uint64_t kNone = 0ULL;
        static const uint64_t kAll = ~kNone;
        uint32_t index = objId / kBitsPerEntry;
        if (!mStorage.get_const(index)) {
            mStorage.add(index, initialIsTrue ? kAll : kNone);
        }
    }

    void remove(uint32_t objId) {
        if (initialIsTrue) {
            set(objId, true);
        } else {
            set(objId, false);
        }
    }

    void set(uint32_t objId, bool predicate) {
        uint32_t index = objId / kBitsPerEntry;

        if (!mStorage.get_const(index)) return;

        uint64_t* current = mStorage.get(index);

        uint64_t flag = 1ULL << (objId % kBitsPerEntry);

        if (predicate) {
            *current = *current | flag;
        } else {
            *current = *current & (~flag);
        }
    }

    bool get(uint32_t objId) const {
        uint32_t index = objId / kBitsPerEntry;

        const uint64_t* current = mStorage.get_const(index);

        if (!current) return initialIsTrue;

        uint64_t flag = 1ULL << (objId % kBitsPerEntry);
        return (flag & (*current)) != 0;
    }

private:
    using Storage = android::base::HybridComponentManager<10000, uint32_t, uint64_t>;
    Storage mStorage;
};

#endif
