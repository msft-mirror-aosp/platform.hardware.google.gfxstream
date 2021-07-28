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

#include "HostUtils.h"

namespace android {
namespace {

bool IsCuttlefish() {
  return android::base::GetProperty("ro.product.board", "") == "cutf";
}

HWC2::Error findCuttlefishDisplayConfigs(std::vector<DisplayConfig>* configs) {
  DEBUG_LOG("%s", __FUNCTION__);

  // TODO: replace with initializing directly from DRM info.
  const auto deviceConfig = cuttlefish::GetDeviceConfig();

  int displayId = 0;
  for (const auto& deviceDisplayConfig : deviceConfig.display_config()) {
    DisplayConfig displayConfig = {
        .id = displayId,
        .width = deviceDisplayConfig.width(),
        .height = deviceDisplayConfig.height(),
        .dpiX = deviceDisplayConfig.dpi(),
        .dpiY = deviceDisplayConfig.dpi(),
        .refreshRateHz = deviceDisplayConfig.refresh_rate_hz(),
    };

    ++displayId;

    configs->push_back(displayConfig);
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

HWC2::Error findGoldfishPrimaryDisplayConfig(
    std::vector<DisplayConfig>* configs) {
  DEBUG_LOG("%s", __FUNCTION__);

  DEFINE_AND_VALIDATE_HOST_CONNECTION
  hostCon->lock();
  const int width = rcEnc->rcGetFBParam(rcEnc, FB_WIDTH);
  const int height = rcEnc->rcGetFBParam(rcEnc, FB_HEIGHT);
  const int dpiX = rcEnc->rcGetFBParam(rcEnc, FB_XDPI);
  const int dpiY = rcEnc->rcGetFBParam(rcEnc, FB_YDPI);
  hostCon->unlock();
  const int refreshRateHz = getVsyncHzFromProperty();

  configs->push_back(DisplayConfig{
      .id = 0,
      .width = width,
      .height = height,
      .dpiX = dpiX,
      .dpiY = dpiY,
      .refreshRateHz = refreshRateHz,
  });

  return HWC2::Error::None;
}

HWC2::Error findGoldfishSecondaryDisplayConfigs(
    std::vector<DisplayConfig>* configs) {
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

  int secondaryDisplayId = 0;
  while (!propIntParts.empty()) {
    configs->push_back(DisplayConfig{
        .id = secondaryDisplayId,
        .width = propIntParts[1],
        .height = propIntParts[2],
        .dpiX = propIntParts[3],
        .dpiY = propIntParts[3],
        .refreshRateHz = 160,
    });

    ++secondaryDisplayId;

    propIntParts.erase(propIntParts.begin(), propIntParts.begin() + 5);
  }

  return HWC2::Error::None;
}

HWC2::Error findGoldfishDisplayConfigs(std::vector<DisplayConfig>* configs) {
  HWC2::Error error = findGoldfishPrimaryDisplayConfig(configs);
  if (error != HWC2::Error::None) {
    ALOGE("%s failed to find Goldfish primary display", __FUNCTION__);
    return error;
  }

  error = findGoldfishSecondaryDisplayConfigs(configs);
  if (error != HWC2::Error::None) {
    ALOGE("%s failed to find Goldfish secondary displays", __FUNCTION__);
  }

  return error;
}

}  // namespace

HWC2::Error findDisplayConfigs(std::vector<DisplayConfig>* configs) {
  if (IsCuttlefish()) {
    return findCuttlefishDisplayConfigs(configs);
  } else {
    return findGoldfishDisplayConfigs(configs);
  }
}

}  // namespace android
