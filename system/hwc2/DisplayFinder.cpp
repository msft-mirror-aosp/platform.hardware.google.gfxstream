/*
 * Copyright 2021 The Android Open Source Project
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

#include "DisplayFinder.h"

#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <device_config_shared.h>

#include "Common.h"
#include "HostUtils.h"

namespace android {
namespace {

HWC2::Error findCuttlefishDisplays(std::vector<DisplayMultiConfigs>& displays) {
  DEBUG_LOG("%s", __FUNCTION__);

  // TODO: replace with initializing directly from DRM info.
  const auto deviceConfig = cuttlefish::GetDeviceConfig();

  int displayId = 0;
  for (const auto& deviceDisplayConfig : deviceConfig.display_config()) {
    DisplayMultiConfigs display = {
        .id = displayId,
        .activeConfigId = 0,
        .configs = {{displayId,
                     deviceDisplayConfig.width(),
                     deviceDisplayConfig.height(),
                     deviceDisplayConfig.dpi(),
                     deviceDisplayConfig.dpi(),
                     deviceDisplayConfig.refresh_rate_hz()}},
    };
    displays.push_back(display);
    ++displayId;
  }

  return HWC2::Error::None;
}

static int getVsyncHzFromProperty() {
  static constexpr const auto kVsyncProp = "ro.boot.qemu.vsync";

  const auto vsyncProp = android::base::GetProperty(kVsyncProp, "");
  DEBUG_LOG("%s: prop value is: %s", __FUNCTION__, vsyncProp.c_str());

  uint64_t vsyncPeriod;
  if (!android::base::ParseUint(vsyncProp, &vsyncPeriod)) {
    ALOGE("%s: failed to parse vsync period '%s', returning default 60",
          __FUNCTION__, vsyncProp.c_str());
    return 60;
  }

  return static_cast<int>(vsyncPeriod);
}

HWC2::Error findGoldfishPrimaryDisplay(std::vector<DisplayMultiConfigs>& displays) {
  DEBUG_LOG("%s", __FUNCTION__);

  DEFINE_AND_VALIDATE_HOST_CONNECTION
  hostCon->lock();
  int activeConfigId;
  const int refreshRateHz = getVsyncHzFromProperty();
  DisplayMultiConfigs display;
  display.id = 0;
  if (rcEnc->hasHWCMultiConfigs()) {
    int count= rcEnc->rcGetFBDisplayConfigsCount(rcEnc);
    if (count <= 0) {
      ALOGE("%s failed to allocate primary display, config count %d", __func__, count);
      return HWC2::Error::NoResources;
    }
    display.activeConfigId = rcEnc->rcGetFBDisplayActiveConfig(rcEnc);
    for(int configId = 0; configId < count; configId++) {
      display.configs.push_back({
                                0,
                                rcEnc->rcGetFBDisplayConfigsParam(
                                    rcEnc, configId, FB_WIDTH),
                                rcEnc->rcGetFBDisplayConfigsParam(
                                    rcEnc, configId, FB_HEIGHT),
                                rcEnc->rcGetFBDisplayConfigsParam(
                                    rcEnc, configId, FB_XDPI),
                                rcEnc->rcGetFBDisplayConfigsParam(
                                    rcEnc, configId, FB_YDPI),
                                refreshRateHz,
                                });
    }
  } else {
    display.activeConfigId = 0;
    display.configs.push_back({
                              0,
                              rcEnc->rcGetFBParam(rcEnc, FB_WIDTH),
                              rcEnc->rcGetFBParam(rcEnc, FB_HEIGHT),
                              rcEnc->rcGetFBParam(rcEnc, FB_XDPI),
                              rcEnc->rcGetFBParam(rcEnc, FB_YDPI),
                              refreshRateHz});
  }
  hostCon->unlock();

  displays.push_back(display);

  return HWC2::Error::None;
}

HWC2::Error findGoldfishSecondaryDisplays(std::vector<DisplayMultiConfigs>& displays) {
  DEBUG_LOG("%s", __FUNCTION__);

  static constexpr const char kExternalDisplayProp[] =
      "hwservicemanager.external.displays";

  const auto propString = android::base::GetProperty(kExternalDisplayProp, "");
  DEBUG_LOG("%s: prop value is: %s", __FUNCTION__, propString.c_str());

  if (propString.empty()) {
    return HWC2::Error::None;
  }

  const std::vector<std::string> propStringParts =
      android::base::Split(propString, ",");
  if (propStringParts.size() % 5 != 0) {
    ALOGE("%s: Invalid syntax for system prop %s which is %s", __FUNCTION__,
          kExternalDisplayProp, propString.c_str());
    return HWC2::Error::BadParameter;
  }

  std::vector<int> propIntParts;
  for (const std::string& propStringPart : propStringParts) {
    int propIntPart;
    if (!android::base::ParseInt(propStringPart, &propIntPart)) {
      ALOGE("%s: Invalid syntax for system prop %s which is %s", __FUNCTION__,
            kExternalDisplayProp, propString.c_str());
      return HWC2::Error::BadParameter;
    }
    propIntParts.push_back(propIntPart);
  }

  int secondaryDisplayId = 1;
  while (!propIntParts.empty()) {
    DisplayMultiConfigs display;
    display.id = secondaryDisplayId;
    display.activeConfigId = 0;
    display.configs.push_back(DisplayConfig{
        .id = secondaryDisplayId,
        .width = propIntParts[1],
        .height = propIntParts[2],
        .dpiX = propIntParts[3],
        .dpiY = propIntParts[3],
        .refreshRateHz = 160,
    });
    displays.push_back(display);

    ++secondaryDisplayId;

    propIntParts.erase(propIntParts.begin(), propIntParts.begin() + 5);
  }

  return HWC2::Error::None;
}

HWC2::Error findGoldfishDisplays(std::vector<DisplayMultiConfigs>& displays) {
  HWC2::Error error = findGoldfishPrimaryDisplay(displays);
  if (error != HWC2::Error::None) {
    ALOGE("%s failed to find Goldfish primary display", __FUNCTION__);
    return error;
  }

  error = findGoldfishSecondaryDisplays(displays);
  if (error != HWC2::Error::None) {
    ALOGE("%s failed to find Goldfish secondary displays", __FUNCTION__);
  }

  return error;
}

}  // namespace

HWC2::Error findDisplays(std::vector<DisplayMultiConfigs>& displays) {
  if (IsCuttlefish()) {
    return findCuttlefishDisplays(displays);
  } else {
    return findGoldfishDisplays(displays);
  }
}

}  // namespace android
