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

#ifndef ANDROID_HWC_DRMPRESENTER_H
#define ANDROID_HWC_DRMPRESENTER_H

#include <map>
#include <memory>
#include <vector>

#include <include/drmhwcgralloc.h>
#include <utils/Thread.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "android/base/synchronization/AndroidLock.h"
#include "Common.h"

namespace android {

class DrmBuffer;
class DrmPresenter;

// A RAII object that will clear a drm framebuffer upon destruction.
class DrmBuffer {
 public:
  DrmBuffer(const native_handle_t* handle, DrmPresenter& drmPresenter);
  ~DrmBuffer();

  DrmBuffer(const DrmBuffer&) = delete;
  DrmBuffer& operator=(const DrmBuffer&) = delete;

  DrmBuffer(DrmBuffer&&) = delete;
  DrmBuffer& operator=(DrmBuffer&&) = delete;

  HWC2::Error flushToDisplay(int display, int* outFlushDoneSyncFd);

 private:
  int convertBoInfo(const native_handle_t* handle);

  DrmPresenter& mDrmPresenter;
  hwc_drm_bo_t mBo;
};

class DrmPresenter {
 public:
  DrmPresenter();
  ~DrmPresenter();

  DrmPresenter(const DrmPresenter&) = delete;
  DrmPresenter& operator=(const DrmPresenter&) = delete;

  DrmPresenter(DrmPresenter&&) = delete;
  DrmPresenter& operator=(DrmPresenter&&) = delete;

  using HotplugCallback =
    std::function<void(bool /*connected*/, uint32_t /*id*/,
                       uint32_t /*width*/, uint32_t /*height*/,
                       uint32_t /*dpiX*/, uint32_t /*dpiY*/,
                       uint32_t /*refreshRate*/)>;

  bool init(const HotplugCallback& cb);

  void clearDrmElementsLocked();
  bool configDrmElementsLocked();

  int getDrmFB(hwc_drm_bo_t& bo);
  int clearDrmFB(hwc_drm_bo_t& bo);

  uint32_t refreshRate() const { return mConnectors[0].mRefreshRateAsInteger; }
  bool handleHotPlug();

  HWC2::Error flushToDisplay(int display, hwc_drm_bo_t& fb, int* outSyncFd);

 private:
  // Drm device.
  int32_t mFd = -1;
  HotplugCallback mHotplugCallback;
  struct DrmPlane {
    uint32_t mId = -1;
    uint32_t mCrtcPropertyId = -1;
    uint32_t mFbPropertyId = -1;
    uint32_t mCrtcXPropertyId = -1;
    uint32_t mCrtcYPropertyId = -1;
    uint32_t mCrtcWPropertyId = -1;
    uint32_t mCrtcHPropertyId = -1;
    uint32_t mSrcXPropertyId = -1;
    uint32_t mSrcYPropertyId = -1;
    uint32_t mSrcWPropertyId = -1;
    uint32_t mSrcHPropertyId = -1;
    uint32_t mTypePropertyId = -1;
    uint64_t mType = -1;
  };
  std::map<uint32_t, DrmPlane> mPlanes;

  struct DrmCrtc {
    uint32_t mId = -1;
    uint32_t mActivePropertyId = -1;
    uint32_t mModePropertyId = -1;
    uint32_t mFencePropertyId = -1;
    uint32_t mPlaneId = -1;

    bool mDidSetCrtc = false;
  };
  std::vector<DrmCrtc> mCrtcs;

  struct DrmConnector {
    uint32_t mId = -1;
    uint32_t mCrtcPropertyId = -1;
    drmModeModeInfo mMode;
    int32_t dpiX;
    int32_t dpiY;
    drmModeConnection connection;
    uint32_t mModeBlobId = 0;
    float mRefreshRateAsFloat;
    uint32_t mRefreshRateAsInteger;
  };
  std::vector<DrmConnector> mConnectors;

  class DrmEventListener : public Thread {
    public:
      DrmEventListener(DrmPresenter& presenter);
      virtual ~DrmEventListener();
    private:
      bool threadLoop() final;
      void UEventHandler();
      void processHotplug(uint64_t timestamp);
      DrmPresenter& mPresenter;
      int mEventFd;
      int mMaxFd;
      fd_set mFds;
  };
  std::unique_ptr<DrmEventListener> mDrmEventListener;
  android::base::guest::ReadWriteLock mStateMutex;
};

}  // namespace android

#endif
