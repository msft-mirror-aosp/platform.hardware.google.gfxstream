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

#include "HostComposer.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <cros_gralloc_handle.h>
#include <drm/virtgpu_drm.h>
#include <poll.h>
#include <sync/sync.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>

#include "../egl/goldfish_sync.h"
#include "Device.h"
#include "Display.h"

namespace android {
namespace {

static int getVsyncHzFromProperty() {
  static constexpr const auto kVsyncProp = "ro.kernel.qemu.vsync";

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

static bool isMinigbmFromProperty() {
  static constexpr const auto kGrallocProp = "ro.hardware.gralloc";

  const auto grallocProp = android::base::GetProperty(kGrallocProp, "");
  DEBUG_LOG("%s: prop value is: %s", __FUNCTION__, grallocProp.c_str());

  if (grallocProp == "minigbm") {
    ALOGD("%s: Using minigbm, in minigbm mode.\n", __FUNCTION__);
    return true;
  } else {
    ALOGD("%s: Is not using minigbm, in goldfish mode.\n", __FUNCTION__);
    return false;
  }
}

#define DEFINE_AND_VALIDATE_HOST_CONNECTION                                   \
  HostConnection* hostCon = createOrGetHostConnection();                      \
  if (!hostCon) {                                                             \
    ALOGE("%s: Failed to get host connection\n", __FUNCTION__);               \
    return HWC2::Error::NoResources;                                          \
  }                                                                           \
  ExtendedRCEncoderContext* rcEnc = hostCon->rcEncoder();                     \
  if (!rcEnc) {                                                               \
    ALOGE("%s: Failed to get renderControl encoder context\n", __FUNCTION__); \
    return HWC2::Error::NoResources;                                          \
  }

static std::unique_ptr<HostConnection> sHostCon;

static HostConnection* createOrGetHostConnection() {
  if (!sHostCon) {
    sHostCon = HostConnection::createUnique();
  }
  return sHostCon.get();
}

typedef struct compose_layer {
  uint32_t cbHandle;
  hwc2_composition_t composeMode;
  hwc_rect_t displayFrame;
  hwc_frect_t crop;
  int32_t blendMode;
  float alpha;
  hwc_color_t color;
  hwc_transform_t transform;
} ComposeLayer;

typedef struct compose_device {
  uint32_t version;
  uint32_t targetHandle;
  uint32_t numLayers;
  struct compose_layer layer[0];
} ComposeDevice;

typedef struct compose_device_v2 {
  uint32_t version;
  uint32_t displayId;
  uint32_t targetHandle;
  uint32_t numLayers;
  struct compose_layer layer[0];
} ComposeDevice_v2;

class ComposeMsg {
 public:
  ComposeMsg(uint32_t layerCnt = 0)
      : mData(sizeof(ComposeDevice) + layerCnt * sizeof(ComposeLayer)) {
    mComposeDevice = reinterpret_cast<ComposeDevice*>(mData.data());
    mLayerCnt = layerCnt;
  }

  ComposeDevice* get() { return mComposeDevice; }

  uint32_t getLayerCnt() { return mLayerCnt; }

 private:
  std::vector<uint8_t> mData;
  uint32_t mLayerCnt;
  ComposeDevice* mComposeDevice;
};

class ComposeMsg_v2 {
 public:
  ComposeMsg_v2(uint32_t layerCnt = 0)
      : mData(sizeof(ComposeDevice_v2) + layerCnt * sizeof(ComposeLayer)) {
    mComposeDevice = reinterpret_cast<ComposeDevice_v2*>(mData.data());
    mLayerCnt = layerCnt;
  }

  ComposeDevice_v2* get() { return mComposeDevice; }

  uint32_t getLayerCnt() { return mLayerCnt; }

 private:
  std::vector<uint8_t> mData;
  uint32_t mLayerCnt;
  ComposeDevice_v2* mComposeDevice;
};

const native_handle_t* AllocateDisplayColorBuffer(int width, int height) {
  const uint32_t layerCount = 1;
  const uint64_t graphicBufferId = 0;  // not used

  buffer_handle_t h;
  uint32_t stride;

  if (GraphicBufferAllocator::get().allocate(
          width, height, PIXEL_FORMAT_RGBA_8888, layerCount,
          (GraphicBuffer::USAGE_HW_COMPOSER | GraphicBuffer::USAGE_HW_RENDER),
          &h, &stride, graphicBufferId, "EmuHWC2") == OK) {
    return static_cast<const native_handle_t*>(h);
  } else {
    return nullptr;
  }
}

void FreeDisplayColorBuffer(const native_handle_t* h) {
  GraphicBufferAllocator::get().free(h);
}

}  // namespace

HostComposer::HostComposer() : mIsMinigbm(isMinigbmFromProperty()) {
  if (!mIsMinigbm) {
    mSyncDeviceFd = goldfish_sync_open();
  }
}

HWC2::Error HostComposer::createDisplays(
    Device* device, const AddDisplayToDeviceFunction& addDisplayToDeviceFn) {
  HWC2::Error error = HWC2::Error::None;

  error = createPrimaryDisplay(device, addDisplayToDeviceFn);
  if (error != HWC2::Error::None) {
    ALOGE("%s failed to create primary display", __FUNCTION__);
    return error;
  }

  error = createSecondaryDisplays(device, addDisplayToDeviceFn);
  if (error != HWC2::Error::None) {
    ALOGE("%s failed to create secondary displays", __FUNCTION__);
    return error;
  }

  return HWC2::Error::None;
}

HWC2::Error HostComposer::createPrimaryDisplay(
    Device* device, const AddDisplayToDeviceFunction& addDisplayToDeviceFn) {
  HWC2::Error error = HWC2::Error::None;

  DEFINE_AND_VALIDATE_HOST_CONNECTION
  hostCon->lock();
  int width = rcEnc->rcGetFBParam(rcEnc, FB_WIDTH);
  int height = rcEnc->rcGetFBParam(rcEnc, FB_HEIGHT);
  int dpiX = rcEnc->rcGetFBParam(rcEnc, FB_XDPI);
  int dpiY = rcEnc->rcGetFBParam(rcEnc, FB_YDPI);
  hostCon->unlock();

  int refreshRateHz = getVsyncHzFromProperty();

  auto display = std::make_unique<Display>(*device, this);
  if (display == nullptr) {
    ALOGE("%s failed to allocate display", __FUNCTION__);
    return HWC2::Error::NoResources;
  }

  auto displayId = display->getId();

  error = display->init(width, height, dpiX, dpiY, refreshRateHz);
  if (error != HWC2::Error::None) {
    ALOGE("%s failed to initialize display:%" PRIu64, __FUNCTION__, displayId);
    return error;
  }

  error = createHostComposerDisplayInfo(display.get(), /*hostDisplayId=*/0);
  if (error != HWC2::Error::None) {
    ALOGE("%s failed to initialize host info for display:%" PRIu64,
          __FUNCTION__, displayId);
    return error;
  }

  error = addDisplayToDeviceFn(std::move(display));
  if (error != HWC2::Error::None) {
    ALOGE("%s failed to add display:%" PRIu64, __FUNCTION__, displayId);
    return error;
  }

  return HWC2::Error::None;
}

HWC2::Error HostComposer::createSecondaryDisplays(
    Device* device, const AddDisplayToDeviceFunction& addDisplayToDeviceFn) {
  HWC2::Error error = HWC2::Error::None;

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
    uint64_t propUintPart;
    if (!android::base::ParseUint(propStringPart, &propUintPart)) {
      ALOGE("%s: Invalid syntax for system prop %s which is %s", __FUNCTION__,
            kExternalDisplayProp, propString.c_str());
      return HWC2::Error::BadParameter;
    }
    propIntParts.push_back(static_cast<int>(propUintPart));
  }

  static constexpr const uint32_t kHostDisplayIdStart = 6;

  uint32_t secondaryDisplayIndex = 0;
  while (!propIntParts.empty()) {
    int width = propIntParts[1];
    int height = propIntParts[2];
    int dpiX = propIntParts[3];
    int dpiY = propIntParts[3];
    int refreshRateHz = 160;

    propIntParts.erase(propIntParts.begin(), propIntParts.begin() + 5);

    uint32_t expectedHostDisplayId =
        kHostDisplayIdStart + secondaryDisplayIndex;
    uint32_t actualHostDisplayId = 0;

    DEFINE_AND_VALIDATE_HOST_CONNECTION
    hostCon->lock();
    rcEnc->rcDestroyDisplay(rcEnc, expectedHostDisplayId);
    rcEnc->rcCreateDisplay(rcEnc, &actualHostDisplayId);
    rcEnc->rcSetDisplayPose(rcEnc, actualHostDisplayId, -1, -1, width, height);
    hostCon->unlock();

    if (actualHostDisplayId != expectedHostDisplayId) {
      ALOGE(
          "Something wrong with host displayId allocation, expected %d "
          "but received %d",
          expectedHostDisplayId, actualHostDisplayId);
    }

    auto display = std::make_unique<Display>(*device, this);
    if (display == nullptr) {
      ALOGE("%s failed to allocate display", __FUNCTION__);
      return HWC2::Error::NoResources;
    }

    auto displayId = display->getId();

    error = display->init(width, height, dpiX, dpiY, refreshRateHz);
    if (error != HWC2::Error::None) {
      ALOGE("%s failed to initialize display:%" PRIu64, __FUNCTION__,
            displayId);
      return error;
    }

    error = createHostComposerDisplayInfo(display.get(), actualHostDisplayId);
    if (error != HWC2::Error::None) {
      ALOGE("%s failed to initialize host info for display:%" PRIu64,
            __FUNCTION__, displayId);
      return error;
    }

    error = addDisplayToDeviceFn(std::move(display));
    if (error != HWC2::Error::None) {
      ALOGE("%s failed to add display:%" PRIu64, __FUNCTION__, displayId);
      return error;
    }
  }

  return HWC2::Error::None;
}

HWC2::Error HostComposer::createHostComposerDisplayInfo(
    Display* display, uint32_t hostDisplayId) {
  HWC2::Error error = HWC2::Error::None;

  hwc2_display_t displayId = display->getId();
  hwc2_config_t displayConfigId;
  int32_t displayWidth;
  int32_t displayHeight;

  error = display->getActiveConfig(&displayConfigId);
  if (error != HWC2::Error::None) {
    ALOGE("%s: display:%" PRIu64 " has no active config", __FUNCTION__,
          displayId);
    return error;
  }

  error = display->getDisplayAttributeEnum(
      displayConfigId, HWC2::Attribute::Width, &displayWidth);
  if (error != HWC2::Error::None) {
    ALOGE("%s: display:%" PRIu64 " failed to get width", __FUNCTION__,
          displayId);
    return error;
  }

  error = display->getDisplayAttributeEnum(
      displayConfigId, HWC2::Attribute::Height, &displayHeight);
  if (error != HWC2::Error::None) {
    ALOGE("%s: display:%" PRIu64 " failed to get height", __FUNCTION__,
          displayId);
    return error;
  }

  auto it = mDisplayInfos.find(displayId);
  if (it != mDisplayInfos.end()) {
    ALOGE("%s: display:%" PRIu64 " already created?", __FUNCTION__, displayId);
  }

  HostComposerDisplayInfo& displayInfo = mDisplayInfos[displayId];

  displayInfo.hostDisplayId = hostDisplayId;

  displayInfo.compositionResultBuffer =
      AllocateDisplayColorBuffer(displayWidth, displayHeight);
  if (displayInfo.compositionResultBuffer == nullptr) {
    ALOGE("%s: display:%" PRIu64 " failed to create target buffer",
          __FUNCTION__, displayId);
    return HWC2::Error::NoResources;
  }

  if (mIsMinigbm) {
    displayInfo.compositionResultDrmBuffer.reset(
        new DrmBuffer(displayInfo.compositionResultBuffer, mVirtioGpu));

    uint32_t vsyncPeriod = 1000 * 1000 * 1000 / mVirtioGpu.refreshRate();
    error = display->setVsyncPeriod(vsyncPeriod);
    if (error != HWC2::Error::None) {
      ALOGE("%s: display:%" PRIu64 " failed to set vsync height", __FUNCTION__,
            displayId);
      return error;
    }
  }

  return HWC2::Error::None;
}

HWC2::Error HostComposer::onDisplayDestroy(Display* display) {
  hwc2_display_t displayId = display->getId();

  auto it = mDisplayInfos.find(displayId);
  if (it == mDisplayInfos.end()) {
    ALOGE("%s: display:%" PRIu64 " missing display buffers?", __FUNCTION__,
          displayId);
    return HWC2::Error::BadDisplay;
  }

  HostComposerDisplayInfo& displayInfo = mDisplayInfos[displayId];

  FreeDisplayColorBuffer(displayInfo.compositionResultBuffer);

  mDisplayInfos.erase(it);

  return HWC2::Error::None;
}

HWC2::Error HostComposer::onDisplayClientTargetSet(Display* display) {
  hwc2_display_t displayId = display->getId();

  auto it = mDisplayInfos.find(displayId);
  if (it == mDisplayInfos.end()) {
    ALOGE("%s: display:%" PRIu64 " missing display buffers?", __FUNCTION__,
          displayId);
    return HWC2::Error::BadDisplay;
  }

  HostComposerDisplayInfo& displayInfo = mDisplayInfos[displayId];

  if (mIsMinigbm) {
    FencedBuffer& clientTargetFencedBuffer = display->getClientTarget();

    displayInfo.clientTargetDrmBuffer.reset(
        new DrmBuffer(clientTargetFencedBuffer.getBuffer(), mVirtioGpu));
  }

  return HWC2::Error::None;
}

HWC2::Error HostComposer::validateDisplay(
    Display* display, std::unordered_map<hwc2_layer_t, HWC2::Composition>*
                          layerCompositionChanges) {
  DEFINE_AND_VALIDATE_HOST_CONNECTION
  hostCon->lock();
  bool hostCompositionV1 = rcEnc->hasHostCompositionV1();
  bool hostCompositionV2 = rcEnc->hasHostCompositionV2();
  hostCon->unlock();

  const std::vector<Layer*> layers = display->getOrderedLayers();

  if (hostCompositionV1 || hostCompositionV2) {
    // Support Device and SolidColor, otherwise, fallback all layers to Client.
    bool fallBack = false;
    for (auto& layer : layers) {
      if (layer->getCompositionType() == HWC2::Composition::Invalid) {
        // Log error for unused layers, layer leak?
        ALOGE("%s layer %u CompositionType(%d) not set", __FUNCTION__,
              (uint32_t)layer->getId(), layer->getCompositionType());
        continue;
      }
      if (layer->getCompositionType() == HWC2::Composition::Client ||
          layer->getCompositionType() == HWC2::Composition::Cursor ||
          layer->getCompositionType() == HWC2::Composition::Sideband) {
        ALOGW("%s: layer %u CompositionType %d, fallback", __FUNCTION__,
              (uint32_t)layer->getId(), layer->getCompositionType());
        fallBack = true;
        break;
      }
    }

    if (display->hasColorTransform()) {
      fallBack = true;
    }
    if (fallBack) {
      for (auto& layer : layers) {
        if (layer->getCompositionType() == HWC2::Composition::Invalid) {
          continue;
        }
        if (layer->getCompositionType() != HWC2::Composition::Client) {
          (*layerCompositionChanges)[layer->getId()] =
              HWC2::Composition::Client;
        }
      }
    }
  } else {
    for (auto& layer : layers) {
      if (layer->getCompositionType() != HWC2::Composition::Client) {
        (*layerCompositionChanges)[layer->getId()] = HWC2::Composition::Client;
      }
    }
  }

  return HWC2::Error::None;
}

HWC2::Error HostComposer::presentDisplay(Display* display,
                                         int32_t* outRetireFence) {
  auto it = mDisplayInfos.find(display->getId());
  if (it == mDisplayInfos.end()) {
    ALOGE("%s: failed to find display buffers for display:%" PRIu64,
          __FUNCTION__, display->getId());
    return HWC2::Error::BadDisplay;
  }

  HostComposerDisplayInfo& displayInfo = it->second;

  DEFINE_AND_VALIDATE_HOST_CONNECTION
  hostCon->lock();
  bool hostCompositionV1 = rcEnc->hasHostCompositionV1();
  bool hostCompositionV2 = rcEnc->hasHostCompositionV2();
  hostCon->unlock();

  // Ff we supports v2, then discard v1
  if (hostCompositionV2) {
    hostCompositionV1 = false;
  }

  const std::vector<Layer*> layers = display->getOrderedLayers();
  if (hostCompositionV2 || hostCompositionV1) {
    uint32_t numLayer = 0;
    for (auto layer : layers) {
      if (layer->getCompositionType() == HWC2::Composition::Device ||
          layer->getCompositionType() == HWC2::Composition::SolidColor) {
        numLayer++;
      }
    }

    DEBUG_LOG("%s: presenting display:%" PRIu64 " with %d layers", __FUNCTION__,
              display->getId(), static_cast<int>(layers.size()));

    display->clearReleaseFencesAndIdsLocked();

    if (numLayer == 0) {
      ALOGW(
          "%s display has no layers to compose, flushing client target buffer.",
          __FUNCTION__);

      FencedBuffer& displayClientTarget = display->getClientTarget();
      if (displayClientTarget.getBuffer() != nullptr) {
        if (mIsMinigbm) {
          int retireFence = displayInfo.clientTargetDrmBuffer->flush();
          *outRetireFence = dup(retireFence);
          close(retireFence);
        } else {
          post(hostCon, rcEnc, displayClientTarget.getBuffer());
          *outRetireFence = displayClientTarget.getFence();
        }
      }
      return HWC2::Error::None;
    }

    std::unique_ptr<ComposeMsg> composeMsg;
    std::unique_ptr<ComposeMsg_v2> composeMsgV2;

    if (hostCompositionV1) {
      composeMsg.reset(new ComposeMsg(numLayer));
    } else {
      composeMsgV2.reset(new ComposeMsg_v2(numLayer));
    }

    // Handle the composition
    ComposeDevice* p;
    ComposeDevice_v2* p2;
    ComposeLayer* l;

    if (hostCompositionV1) {
      p = composeMsg->get();
      l = p->layer;
    } else {
      p2 = composeMsgV2->get();
      l = p2->layer;
    }

    int releaseLayersCount = 0;
    for (auto layer : layers) {
      if (layer->getCompositionType() != HWC2::Composition::Device &&
          layer->getCompositionType() != HWC2::Composition::SolidColor) {
        ALOGE("%s: Unsupported composition types %d layer %u", __FUNCTION__,
              layer->getCompositionType(), (uint32_t)layer->getId());
        continue;
      }
      // send layer composition command to host
      if (layer->getCompositionType() == HWC2::Composition::Device) {
        display->addReleaseLayerLocked(layer->getId());
        releaseLayersCount++;

        int fence = layer->getBuffer().getFence();
        if (fence != -1) {
          int err = sync_wait(fence, 3000);
          if (err < 0 && errno == ETIME) {
            ALOGE("%s waited on fence %d for 3000 ms", __FUNCTION__, fence);
          }
          close(fence);
        } else {
          ALOGV("%s: acquire fence not set for layer %u", __FUNCTION__,
                (uint32_t)layer->getId());
        }
        const native_handle_t* cb = layer->getBuffer().getBuffer();
        if (cb != nullptr) {
          l->cbHandle = hostCon->grallocHelper()->getHostHandle(cb);
        } else {
          ALOGE("%s null buffer for layer %d", __FUNCTION__,
                (uint32_t)layer->getId());
        }
      } else {
        // solidcolor has no buffer
        l->cbHandle = 0;
      }
      l->composeMode = (hwc2_composition_t)layer->getCompositionType();
      l->displayFrame = layer->getDisplayFrame();
      l->crop = layer->getSourceCrop();
      l->blendMode = static_cast<int32_t>(layer->getBlendMode());
      l->alpha = layer->getPlaneAlpha();
      l->color = layer->getColor();
      l->transform = layer->getTransform();
      ALOGV(
          "   cb %d blendmode %d alpha %f %d %d %d %d z %d"
          " composeMode %d, transform %d",
          l->cbHandle, l->blendMode, l->alpha, l->displayFrame.left,
          l->displayFrame.top, l->displayFrame.right, l->displayFrame.bottom,
          layer->getZ(), l->composeMode, l->transform);
      l++;
    }
    if (hostCompositionV1) {
      p->version = 1;
      p->targetHandle = hostCon->grallocHelper()->getHostHandle(
          displayInfo.compositionResultBuffer);
      p->numLayers = numLayer;
    } else {
      p2->version = 2;
      p2->displayId = displayInfo.hostDisplayId;
      p2->targetHandle = hostCon->grallocHelper()->getHostHandle(
          displayInfo.compositionResultBuffer);
      p2->numLayers = numLayer;
    }

    hostCon->lock();
    if (rcEnc->hasAsyncFrameCommands()) {
      if (mIsMinigbm) {
        if (hostCompositionV1) {
          rcEnc->rcComposeAsyncWithoutPost(
              rcEnc, sizeof(ComposeDevice) + numLayer * sizeof(ComposeLayer),
              (void*)p);
        } else {
          rcEnc->rcComposeAsyncWithoutPost(
              rcEnc, sizeof(ComposeDevice_v2) + numLayer * sizeof(ComposeLayer),
              (void*)p2);
        }
      } else {
        if (hostCompositionV1) {
          rcEnc->rcComposeAsync(
              rcEnc, sizeof(ComposeDevice) + numLayer * sizeof(ComposeLayer),
              (void*)p);
        } else {
          rcEnc->rcComposeAsync(
              rcEnc, sizeof(ComposeDevice_v2) + numLayer * sizeof(ComposeLayer),
              (void*)p2);
        }
      }
    } else {
      if (mIsMinigbm) {
        if (hostCompositionV1) {
          rcEnc->rcComposeWithoutPost(
              rcEnc, sizeof(ComposeDevice) + numLayer * sizeof(ComposeLayer),
              (void*)p);
        } else {
          rcEnc->rcComposeWithoutPost(
              rcEnc, sizeof(ComposeDevice_v2) + numLayer * sizeof(ComposeLayer),
              (void*)p2);
        }
      } else {
        if (hostCompositionV1) {
          rcEnc->rcCompose(
              rcEnc, sizeof(ComposeDevice) + numLayer * sizeof(ComposeLayer),
              (void*)p);
        } else {
          rcEnc->rcCompose(
              rcEnc, sizeof(ComposeDevice_v2) + numLayer * sizeof(ComposeLayer),
              (void*)p2);
        }
      }
    }

    hostCon->unlock();

    // Send a retire fence and use it as the release fence for all layers,
    // since media expects it
    EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_ANDROID,
                        EGL_NO_NATIVE_FENCE_FD_ANDROID};

    uint64_t sync_handle, thread_handle;
    int retire_fd;

    hostCon->lock();
    rcEnc->rcCreateSyncKHR(rcEnc, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs,
                           2 * sizeof(EGLint), true /* destroy when signaled */,
                           &sync_handle, &thread_handle);
    hostCon->unlock();

    if (mIsMinigbm) {
      retire_fd = -1;
      retire_fd = displayInfo.compositionResultDrmBuffer->flush();
    } else {
      goldfish_sync_queue_work(mSyncDeviceFd, sync_handle, thread_handle,
                               &retire_fd);
    }

    for (size_t i = 0; i < releaseLayersCount; ++i) {
      display->addReleaseFenceLocked(dup(retire_fd));
    }

    *outRetireFence = dup(retire_fd);
    close(retire_fd);
    hostCon->lock();
    if (rcEnc->hasAsyncFrameCommands()) {
      rcEnc->rcDestroySyncKHRAsync(rcEnc, sync_handle);
    } else {
      rcEnc->rcDestroySyncKHR(rcEnc, sync_handle);
    }
    hostCon->unlock();

  } else {
    // we set all layers Composition::Client, so do nothing.
    if (mIsMinigbm) {
      int retireFence = displayInfo.clientTargetDrmBuffer->flush();
      *outRetireFence = dup(retireFence);
      close(retireFence);
    } else {
      FencedBuffer& displayClientTarget = display->getClientTarget();
      post(hostCon, rcEnc, displayClientTarget.getBuffer());
      *outRetireFence = displayClientTarget.getFence();
    }
    ALOGV("%s fallback to post, returns outRetireFence %d", __FUNCTION__,
          *outRetireFence);
  }

  return HWC2::Error::None;
}

void HostComposer::post(HostConnection* hostCon,
                        ExtendedRCEncoderContext* rcEnc, buffer_handle_t h) {
  assert(cb && "native_handle_t::from(h) failed");

  hostCon->lock();
  rcEnc->rcFBPost(rcEnc, hostCon->grallocHelper()->getHostHandle(h));
  hostCon->flush();
  hostCon->unlock();
}

HostComposer::VirtioGpu::VirtioGpu() {
  drmModeRes* res;
  drmModeConnector* conn;

  mFd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (mFd < 0) {
    ALOGE("%s HWC2::Error opening virtioGPU device: %d", __FUNCTION__, errno);
    return;
  }

  int univRet = drmSetClientCap(mFd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (univRet) {
    ALOGE("%s: fail to set universal plane %d\n", __FUNCTION__, univRet);
  }

  int atomicRet = drmSetClientCap(mFd, DRM_CLIENT_CAP_ATOMIC, 1);

  if (atomicRet) {
    ALOGE("%s: fail to set atomic operation %d, %d\n", __FUNCTION__, atomicRet,
          errno);
  }

  ALOGD("%s: Did set universal planes and atomic cap\n", __FUNCTION__);

  res = drmModeGetResources(mFd);
  if (res == nullptr) {
    ALOGE("%s HWC2::Error reading drm resources: %d", __FUNCTION__, errno);
    close(mFd);
    mFd = -1;
    return;
  }

  mCrtcId = res->crtcs[0];
  mConnectorId = res->connectors[0];

  drmModePlaneResPtr plane_res = drmModeGetPlaneResources(mFd);
  for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
    drmModePlanePtr plane = drmModeGetPlane(mFd, plane_res->planes[i]);
    ALOGD("%s: plane id: %u crtcid %u fbid %u crtc xy %d %d xy %d %d\n",
          __FUNCTION__, plane->plane_id, plane->crtc_id, plane->fb_id,
          plane->crtc_x, plane->crtc_y, plane->x, plane->y);

    drmModeObjectPropertiesPtr planeProps =
        drmModeObjectGetProperties(mFd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
    bool found = false;
    bool isPrimaryOrOverlay = false;
    for (int i = 0; !found && (size_t)i < planeProps->count_props; ++i) {
      drmModePropertyPtr p = drmModeGetProperty(mFd, planeProps->props[i]);
      if (!strcmp(p->name, "CRTC_ID")) {
        mPlaneCrtcPropertyId = p->prop_id;
        ALOGD("%s: Found plane crtc property id. id: %u\n", __FUNCTION__,
              mPlaneCrtcPropertyId);
      } else if (!strcmp(p->name, "FB_ID")) {
        mPlaneFbPropertyId = p->prop_id;
        ALOGD("%s: Found plane fb property id. id: %u\n", __FUNCTION__,
              mPlaneFbPropertyId);
      } else if (!strcmp(p->name, "CRTC_X")) {
        mPlaneCrtcXPropertyId = p->prop_id;
        ALOGD("%s: Found plane crtc X property id. id: %u\n", __FUNCTION__,
              mPlaneCrtcXPropertyId);
      } else if (!strcmp(p->name, "CRTC_Y")) {
        mPlaneCrtcYPropertyId = p->prop_id;
        ALOGD("%s: Found plane crtc Y property id. id: %u\n", __FUNCTION__,
              mPlaneCrtcYPropertyId);
      } else if (!strcmp(p->name, "CRTC_W")) {
        mPlaneCrtcWPropertyId = p->prop_id;
        ALOGD("%s: Found plane crtc W property id. id: %u value: %u\n",
              __FUNCTION__, mPlaneCrtcWPropertyId, (uint32_t)p->values[0]);
      } else if (!strcmp(p->name, "CRTC_H")) {
        mPlaneCrtcHPropertyId = p->prop_id;
        ALOGD("%s: Found plane crtc H property id. id: %u value: %u\n",
              __FUNCTION__, mPlaneCrtcHPropertyId, (uint32_t)p->values[0]);
      } else if (!strcmp(p->name, "SRC_X")) {
        mPlaneSrcXPropertyId = p->prop_id;
        ALOGD("%s: Found plane src X property id. id: %u\n", __FUNCTION__,
              mPlaneSrcXPropertyId);
      } else if (!strcmp(p->name, "SRC_Y")) {
        mPlaneSrcYPropertyId = p->prop_id;
        ALOGD("%s: Found plane src Y property id. id: %u\n", __FUNCTION__,
              mPlaneSrcYPropertyId);
      } else if (!strcmp(p->name, "SRC_W")) {
        mPlaneSrcWPropertyId = p->prop_id;
        ALOGD("%s: Found plane src W property id. id: %u\n", __FUNCTION__,
              mPlaneSrcWPropertyId);
      } else if (!strcmp(p->name, "SRC_H")) {
        mPlaneSrcHPropertyId = p->prop_id;
        ALOGD("%s: Found plane src H property id. id: %u\n", __FUNCTION__,
              mPlaneSrcHPropertyId);
      } else if (!strcmp(p->name, "type")) {
        mPlaneTypePropertyId = p->prop_id;
        ALOGD("%s: Found plane type property id. id: %u\n", __FUNCTION__,
              mPlaneTypePropertyId);
        ALOGD("%s: Plane property type value 0x%llx\n", __FUNCTION__,
              (unsigned long long)p->values[0]);
        uint64_t type = p->values[0];
        switch (type) {
          case DRM_PLANE_TYPE_OVERLAY:
          case DRM_PLANE_TYPE_PRIMARY:
            isPrimaryOrOverlay = true;
            ALOGD(
                "%s: Found a primary or overlay plane. plane id: %u type "
                "0x%llx\n",
                __FUNCTION__, plane->plane_id, (unsigned long long)type);
            break;
          default:
            break;
        }
      }
      drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(planeProps);

    if (isPrimaryOrOverlay && ((1 << 0) & plane->possible_crtcs)) {
      mPlaneId = plane->plane_id;
      ALOGD("%s: found plane compatible with crtc id %d: %d\n", __FUNCTION__,
            mCrtcId, mPlaneId);
      drmModeFreePlane(plane);
      break;
    }

    drmModeFreePlane(plane);
  }
  drmModeFreePlaneResources(plane_res);

  conn = drmModeGetConnector(mFd, mConnectorId);
  if (conn == nullptr) {
    ALOGE("%s HWC2::Error reading drm connector %d: %d", __FUNCTION__,
          mConnectorId, errno);
    drmModeFreeResources(res);
    close(mFd);
    mFd = -1;
    return;
  }
  memcpy(&mMode, &conn->modes[0], sizeof(drmModeModeInfo));

  drmModeCreatePropertyBlob(mFd, &mMode, sizeof(mMode), &mModeBlobId);

  mRefreshRateAsFloat =
      1000.0f * mMode.clock / ((float)mMode.vtotal * (float)mMode.htotal);
  mRefreshRateAsInteger = (uint32_t)(mRefreshRateAsFloat + 0.5f);

  ALOGD(
      "%s: using drm init. refresh rate of system is %f, rounding to %d. blob "
      "id %d\n",
      __FUNCTION__, mRefreshRateAsFloat, mRefreshRateAsInteger, mModeBlobId);

  {
    drmModeObjectPropertiesPtr connectorProps = drmModeObjectGetProperties(
        mFd, mConnectorId, DRM_MODE_OBJECT_CONNECTOR);
    bool found = false;
    for (int i = 0; !found && (size_t)i < connectorProps->count_props; ++i) {
      drmModePropertyPtr p = drmModeGetProperty(mFd, connectorProps->props[i]);
      if (!strcmp(p->name, "CRTC_ID")) {
        mConnectorCrtcPropertyId = p->prop_id;
        ALOGD("%s: Found connector crtc id prop id: %u\n", __FUNCTION__,
              mConnectorCrtcPropertyId);
        found = true;
      }
      drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(connectorProps);
  }

  {
    drmModeObjectPropertiesPtr crtcProps =
        drmModeObjectGetProperties(mFd, mCrtcId, DRM_MODE_OBJECT_CRTC);
    bool found = false;
    for (int i = 0; !found && (size_t)i < crtcProps->count_props; ++i) {
      drmModePropertyPtr p = drmModeGetProperty(mFd, crtcProps->props[i]);
      if (!strcmp(p->name, "OUT_FENCE_PTR")) {
        mOutFencePtrId = p->prop_id;
        ALOGD("%s: Found out fence ptr id. id: %u\n", __FUNCTION__,
              mOutFencePtrId);
      } else if (!strcmp(p->name, "ACTIVE")) {
        mCrtcActivePropretyId = p->prop_id;
        ALOGD("%s: Found out crtc active prop id %u\n", __FUNCTION__,
              mCrtcActivePropretyId);
      } else if (!strcmp(p->name, "MODE_ID")) {
        mCrtcModeIdPropertyId = p->prop_id;
        ALOGD("%s: Found out crtc mode id prop id %u\n", __FUNCTION__,
              mCrtcModeIdPropertyId);
      }
      drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(crtcProps);
  }

  drmModeFreeConnector(conn);
  drmModeFreeResources(res);
  ALOGD("%s: Successfully initialized DRM backend", __FUNCTION__);
}

HostComposer::VirtioGpu::~VirtioGpu() { close(mFd); }

int HostComposer::VirtioGpu::setCrtc(hwc_drm_bo_t& bo) {
  int ret =
      drmModeSetCrtc(mFd, mCrtcId, bo.fb_id, 0, 0, &mConnectorId, 1, &mMode);
  ALOGV("%s: drm FB %d", __FUNCTION__, bo.fb_id);
  if (ret) {
    ALOGE("%s: drmModeSetCrtc failed: %s (errno %d)", __FUNCTION__,
          strerror(errno), errno);
    return -1;
  }
  return 0;
}

int HostComposer::VirtioGpu::getDrmFB(hwc_drm_bo_t& bo) {
  int ret = drmPrimeFDToHandle(mFd, bo.prime_fds[0], &bo.gem_handles[0]);
  if (ret) {
    ALOGE("%s: drmPrimeFDToHandle failed: %s (errno %d)", __FUNCTION__,
          strerror(errno), errno);
    return -1;
  }
  ret = drmModeAddFB2(mFd, bo.width, bo.height, bo.format, bo.gem_handles,
                      bo.pitches, bo.offsets, &bo.fb_id, 0);
  if (ret) {
    ALOGE("%s: drmModeAddFB2 failed: %s (errno %d)", __FUNCTION__,
          strerror(errno), errno);
    return -1;
  }
  ALOGV("%s: drm FB %d", __FUNCTION__, bo.fb_id);
  return 0;
}

int HostComposer::VirtioGpu::clearDrmFB(hwc_drm_bo_t& bo) {
  int ret = 0;
  if (bo.fb_id) {
    if (drmModeRmFB(mFd, bo.fb_id)) {
      ALOGE("%s: drmModeRmFB failed: %s (errno %d)", __FUNCTION__,
            strerror(errno), errno);
    }
    ret = -1;
  }
  if (bo.gem_handles[0]) {
    struct drm_gem_close gem_close = {};
    gem_close.handle = bo.gem_handles[0];
    if (drmIoctl(mFd, DRM_IOCTL_GEM_CLOSE, &gem_close)) {
      ALOGE("%s: DRM_IOCTL_GEM_CLOSE failed: %s (errno %d)", __FUNCTION__,
            strerror(errno), errno);
    }
    ret = -1;
  }
  ALOGV("%s: drm FB %d", __FUNCTION__, bo.fb_id);
  return ret;
}

bool HostComposer::VirtioGpu::supportComposeWithoutPost() { return true; }

int HostComposer::VirtioGpu::exportSyncFdAndSetCrtc(hwc_drm_bo_t& bo) {
  mOutFence = -1;

  drmModeAtomicReqPtr pset = drmModeAtomicAlloc();

  int ret;

  if (!mDidSetCrtc) {
    DEBUG_LOG("%s: Setting crtc.\n", __FUNCTION__);
    ret = drmModeAtomicAddProperty(pset, mCrtcId, mCrtcActivePropretyId, 1);
    if (ret < 0) {
      ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
    }
    ret = drmModeAtomicAddProperty(pset, mCrtcId, mCrtcModeIdPropertyId,
                                   mModeBlobId);
    if (ret < 0) {
      ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
    }
    ret = drmModeAtomicAddProperty(pset, mConnectorId, mConnectorCrtcPropertyId,
                                   mCrtcId);
    if (ret < 0) {
      ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
    }

    mDidSetCrtc = true;
  } else {
    DEBUG_LOG("%s: Already set crtc\n", __FUNCTION__);
  }

  ret = drmModeAtomicAddProperty(pset, mCrtcId, mOutFencePtrId,
                                 (uint64_t)(&mOutFence));
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }

  DEBUG_LOG("%s: set plane: plane id %d crtcid %d fbid %d bo w h %d %d\n",
            __FUNCTION__, mPlaneId, mCrtcId, bo.fb_id, bo.width, bo.height);

  ret = drmModeAtomicAddProperty(pset, mPlaneId, mPlaneCrtcPropertyId, mCrtcId);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, mPlaneId, mPlaneFbPropertyId, bo.fb_id);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, mPlaneId, mPlaneCrtcXPropertyId, 0);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, mPlaneId, mPlaneCrtcYPropertyId, 0);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret =
      drmModeAtomicAddProperty(pset, mPlaneId, mPlaneCrtcWPropertyId, bo.width);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, mPlaneId, mPlaneCrtcHPropertyId,
                                 bo.height);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, mPlaneId, mPlaneSrcXPropertyId, 0);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, mPlaneId, mPlaneSrcYPropertyId, 0);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, mPlaneId, mPlaneSrcWPropertyId,
                                 bo.width << 16);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, mPlaneId, mPlaneSrcHPropertyId,
                                 bo.height << 16);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }

  uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
  ret = drmModeAtomicCommit(mFd, pset, flags, 0);

  if (ret) {
    ALOGE("%s: Atomic commit failed with %d %d\n", __FUNCTION__, ret, errno);
  }

  if (pset) drmModeAtomicFree(pset);

  DEBUG_LOG("%s: out fence: %d\n", __FUNCTION__, mOutFence);
  return mOutFence;
}

HostComposer::DrmBuffer::DrmBuffer(const native_handle_t* handle,
                                   VirtioGpu& virtioGpu)
    : mVirtioGpu(virtioGpu), mBo({}) {
  if (!convertBoInfo(handle)) {
    mVirtioGpu.getDrmFB(mBo);
  }
}

HostComposer::DrmBuffer::~DrmBuffer() { mVirtioGpu.clearDrmFB(mBo); }

int HostComposer::DrmBuffer::convertBoInfo(const native_handle_t* handle) {
  cros_gralloc_handle* gr_handle = (cros_gralloc_handle*)handle;
  if (!gr_handle) {
    ALOGE("%s: Null buffer handle", __FUNCTION__);
    return -1;
  }
  mBo.width = gr_handle->width;
  mBo.height = gr_handle->height;
  mBo.hal_format = gr_handle->droid_format;
  mBo.format = gr_handle->format;
  mBo.usage = gr_handle->usage;
  mBo.prime_fds[0] = gr_handle->fds[0];
  mBo.pitches[0] = gr_handle->strides[0];
  return 0;
}

int HostComposer::DrmBuffer::flush() {
  return mVirtioGpu.exportSyncFdAndSetCrtc(mBo);
}

}  // namespace android