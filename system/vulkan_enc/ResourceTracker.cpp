// Copyright (C) 2018 The Android Open Source Project
// Copyright (C) 2018 Google Inc.
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

#include "ResourceTracker.h"

#include "Resources.h"

namespace goldfish_vk {

#define MAKE_HANDLE_MAPPING_FOREACH(type_name, map_impl, map_to_u64_impl, map_from_u64_impl) \
    void mapHandles_##type_name(type_name* handles, size_t count) override { \
        for (size_t i = 0; i < count; ++i) { \
            map_impl; \
        } \
    } \
    void mapHandles_##type_name##_u64(const type_name* handles, uint64_t* handle_u64s, size_t count) override { \
        for (size_t i = 0; i < count; ++i) { \
            map_to_u64_impl; \
        } \
    } \
    void mapHandles_u64_##type_name(const uint64_t* handle_u64s, type_name* handles, size_t count) override { \
        for (size_t i = 0; i < count; ++i) { \
            map_from_u64_impl; \
        } \
    } \

#define DEFINE_RESOURCE_TRACKING_CLASS(class_name, impl) \
class class_name : public VulkanHandleMapping { \
public: \
    virtual ~class_name() { } \
    GOLDFISH_VK_LIST_HANDLE_TYPES(impl) \
}; \

#define CREATE_MAPPING_IMPL_FOR_TYPE(type_name) \
    MAKE_HANDLE_MAPPING_FOREACH(type_name, \
        handles[i] = new_from_host_##type_name(handles[i]), \
        handle_u64s[i] = (uint64_t)(uintptr_t)new_from_host_##type_name(handles[i]), \
        handles[i] = (type_name)(uintptr_t)new_from_host_u64_##type_name(handle_u64s[i]))

#define UNWRAP_MAPPING_IMPL_FOR_TYPE(type_name) \
    MAKE_HANDLE_MAPPING_FOREACH(type_name, \
        handles[i] = get_host_##type_name(handles[i]), \
        handle_u64s[i] = (uint64_t)(uintptr_t)get_host_u64_##type_name(handles[i]), \
        handles[i] = (type_name)(uintptr_t)get_host_##type_name((type_name)(uintptr_t)handle_u64s[i]))

#define DESTROY_MAPPING_IMPL_FOR_TYPE(type_name) \
    MAKE_HANDLE_MAPPING_FOREACH(type_name, \
        delete_goldfish_##type_name(handles[i]), \
        (void)handle_u64s[i]; delete_goldfish_##type_name(handles[i]), \
        (void)handles[i]; delete_goldfish_##type_name((type_name)(uintptr_t)handle_u64s[i]))

DEFINE_RESOURCE_TRACKING_CLASS(CreateMapping, CREATE_MAPPING_IMPL_FOR_TYPE)
DEFINE_RESOURCE_TRACKING_CLASS(UnwrapMapping, UNWRAP_MAPPING_IMPL_FOR_TYPE)
DEFINE_RESOURCE_TRACKING_CLASS(DestroyMapping, DESTROY_MAPPING_IMPL_FOR_TYPE)

class ResourceTracker::Impl {
public:
    Impl() = default;
    CreateMapping createMapping;
    UnwrapMapping unwrapMapping;
    DestroyMapping destroyMapping;
    DefaultHandleMapping defaultMapping;
};
ResourceTracker::ResourceTracker() : mImpl(new ResourceTracker::Impl()) { }
ResourceTracker::~ResourceTracker() { }
VulkanHandleMapping* ResourceTracker::createMapping() {
    return &mImpl->createMapping;
}
VulkanHandleMapping* ResourceTracker::unwrapMapping() {
    return &mImpl->unwrapMapping;
}
VulkanHandleMapping* ResourceTracker::destroyMapping() {
    return &mImpl->destroyMapping;
}
VulkanHandleMapping* ResourceTracker::defaultMapping() {
    return &mImpl->defaultMapping;
}
static ResourceTracker* sTracker = nullptr;
// static
ResourceTracker* ResourceTracker::get() {
    if (!sTracker) {
        // To be initialized once on vulkan device open.
        sTracker = new ResourceTracker;
    }
    return sTracker;
}

} // namespace goldfish_vk
