/*
 * Copyright 2022 The Android Open Source Project
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

#include "DrmConnector.h"

namespace aidl::android::hardware::graphics::composer3::impl {
namespace {

static constexpr const int32_t kUmPerInch = 25400;

}  // namespace

std::unique_ptr<DrmConnector> DrmConnector::create(::android::base::borrowed_fd drmFd,
                                                   uint32_t connectorId) {
    std::unique_ptr<DrmConnector> connector(new DrmConnector(connectorId));

    if (!LoadDrmProperties(drmFd, connectorId, DRM_MODE_OBJECT_CONNECTOR, GetPropertiesMap(),
                           connector.get())) {
        ALOGE("%s: Failed to load connector properties.", __FUNCTION__);
        return nullptr;
    }

    if (!connector->update(drmFd)) {
        return nullptr;
    }

    return connector;
}

bool DrmConnector::update(::android::base::borrowed_fd drmFd) {
    DEBUG_LOG("%s: Loading properties for connector:%" PRIu32, __FUNCTION__, mId);

    drmModeConnector* drmConnector = drmModeGetConnector(drmFd.get(), mId);
    if (!drmConnector) {
        ALOGE("%s: Failed to load connector.", __FUNCTION__);
        return false;
    }

    mStatus = drmConnector->connection;

    mModes.clear();
    for (uint32_t i = 0; i < drmConnector->count_modes; i++) {
        auto mode = DrmMode::create(drmFd, drmConnector->modes[i]);
        if (!mode) {
            ALOGE("%s: Failed to create mode for connector.", __FUNCTION__);
            return false;
        }

        mModes.push_back(std::move(mode));
    }

    mWidthMillimeters = drmConnector->mmWidth;
    mHeightMillimeters = drmConnector->mmHeight;

    drmModeFreeConnector(drmConnector);

    return true;
}

uint32_t DrmConnector::getWidth() const {
    if (mModes.empty()) {
        return 0;
    }
    return mModes[0]->hdisplay;
}

uint32_t DrmConnector::getHeight() const {
    if (mModes.empty()) {
        return 0;
    }
    return mModes[0]->vdisplay;
}

int32_t DrmConnector::getDpiX() const {
    if (mModes.empty()) {
        return -1;
    }

    const auto& mode = mModes[0];
    if (mWidthMillimeters) {
        return (mode->hdisplay * kUmPerInch) / mWidthMillimeters;
    }

    return -1;
}

int32_t DrmConnector::getDpiY() const {
    if (mModes.empty()) {
        return -1;
    }

    const auto& mode = mModes[0];
    if (mHeightMillimeters) {
        return (mode->hdisplay * kUmPerInch) / mHeightMillimeters;
    }

    return -1;
}

float DrmConnector::getRefreshRate() const {
    if (!mModes.empty()) {
        const auto& mode = mModes[0];
        return 1000.0f * mode->clock / ((float)mode->vtotal * (float)mode->htotal);
    }

    return -1.0f;
}

}  // namespace aidl::android::hardware::graphics::composer3::impl
