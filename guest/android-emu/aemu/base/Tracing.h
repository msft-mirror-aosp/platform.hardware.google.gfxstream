// Copyright (C) 2019 The Android Open Source Project
// Copyright (C) 2019 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

// Library to perform tracing. Talks to platform-specific
// tracing libraries.

namespace gfxstream {
namespace guest {

#ifdef HOST_BUILD
void initializeTracing();
void enableTracing();
void disableTracing();
// Some platform tracing libraries such as Perfetto can be enabled/disabled at
// runtime. Allow the user to query if they are disabled or not, and take
// further action based on it. The use case is to enable/disable tracing on the
// host alongside.
bool isTracingEnabled();

class ScopedTrace {
public:
    ScopedTrace(const char* name);
    ~ScopedTrace();
};

class ScopedTraceDerived : public ScopedTrace {
public:
    void* member = nullptr;
};
#endif

bool isTracingEnabled();

class ScopedTraceGuest {
public:
    ScopedTraceGuest(const char* name) : name_(name) {
        beginTraceImpl(name_);
    }

    ~ScopedTraceGuest() {
        endTraceImpl(name_);
    }
private:
    void beginTraceImpl(const char* name);
    void endTraceImpl(const char* name);

    const char* const name_;
};

} // namespace base
} // namespace android

#define __AEMU_GENSYM2(x,y) x##y
#define __AEMU_GENSYM1(x,y) __AEMU_GENSYM2(x,y)
#define AEMU_GENSYM(x) __AEMU_GENSYM1(x,__COUNTER__)

#ifdef HOST_BUILD
#define AEMU_SCOPED_TRACE(tag) __attribute__ ((unused)) gfxstream::guest::ScopedTrace AEMU_GENSYM(aemuScopedTrace_)(tag)
#else
#define AEMU_SCOPED_TRACE(tag) __attribute__ ((unused)) gfxstream::guest::ScopedTraceGuest AEMU_GENSYM(aemuScopedTrace_)(tag)
#endif
