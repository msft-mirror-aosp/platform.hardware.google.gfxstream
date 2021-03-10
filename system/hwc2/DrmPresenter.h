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

#include <include/drmhwcgralloc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

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

  int flush();

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

  bool init();

  int setCrtc(hwc_drm_bo_t& fb);
  int getDrmFB(hwc_drm_bo_t& bo);
  int clearDrmFB(hwc_drm_bo_t& bo);
  bool supportComposeWithoutPost();
  uint32_t refreshRate() const { return mRefreshRateAsInteger; }

  int exportSyncFdAndSetCrtc(hwc_drm_bo_t& fb);

 private:
  drmModeModeInfo mMode;

  int32_t mFd = -1;
  uint32_t mConnectorId;
  uint32_t mCrtcId;

  uint32_t mConnectorCrtcPropertyId;

  uint32_t mOutFencePtrId;
  uint32_t mCrtcActivePropretyId;
  uint32_t mCrtcModeIdPropertyId;
  uint32_t mModeBlobId;

  uint32_t mPlaneId;
  uint32_t mPlaneCrtcPropertyId;
  uint32_t mPlaneFbPropertyId;
  uint32_t mPlaneCrtcXPropertyId;
  uint32_t mPlaneCrtcYPropertyId;
  uint32_t mPlaneCrtcWPropertyId;
  uint32_t mPlaneCrtcHPropertyId;
  uint32_t mPlaneSrcXPropertyId;
  uint32_t mPlaneSrcYPropertyId;
  uint32_t mPlaneSrcWPropertyId;
  uint32_t mPlaneSrcHPropertyId;
  uint32_t mPlaneTypePropertyId;

  float mRefreshRateAsFloat;
  uint32_t mRefreshRateAsInteger;

  int mOutFence = -1;

  bool mDidSetCrtc = false;
};

}  // namespace android

#endif