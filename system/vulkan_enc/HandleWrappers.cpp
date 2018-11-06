// Copyright (C) 2018 The Android Open Source Project
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
#include "HandleWrappers.h"

#include <stdlib.h>

extern "C" {

#define GOLDFISH_VK_NEW_FROM_HOST_IMPL(type) \
    type new_from_host_##type(type underlying) { \
        struct goldfish_##type* res = \
            static_cast<goldfish_##type*>(malloc(sizeof(goldfish_##type))); \
        res->dispatch.magic = HWVULKAN_DISPATCH_MAGIC; \
        res->dispatch.vtbl = nullptr; \
        res->underlying = underlying; \
        return reinterpret_cast<type>(res); \
    } \

#define GOLDFISH_VK_AS_GOLDFISH_IMPL(type) \
    struct goldfish_##type* as_goldfish_##type(type toCast) { \
        return reinterpret_cast<goldfish_##type*>(toCast); \
    } \

#define GOLDFISH_VK_GET_HOST_IMPL(type) \
    type get_host_##type(type toUnwrap) { \
        auto as_goldfish = as_goldfish_##type(toUnwrap); \
        return as_goldfish->underlying; \
    } \

#define GOLDFISH_VK_DELETE_GOLDFISH_IMPL(type) \
    void delete_goldfish_##type(type toDelete) { \
        free(as_goldfish_##type(toDelete)); \
    } \

GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPESS(GOLDFISH_VK_NEW_FROM_HOST_IMPL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPESS(GOLDFISH_VK_AS_GOLDFISH_IMPL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPESS(GOLDFISH_VK_GET_HOST_IMPL)
GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPESS(GOLDFISH_VK_DELETE_GOLDFISH_IMPL)

} // extern "C"