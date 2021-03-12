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

#include "DrmPresenter.h"

#include <cros_gralloc_handle.h>

namespace android {

DrmPresenter::DrmPresenter() {}

bool DrmPresenter::init() {
  DEBUG_LOG("%s", __FUNCTION__);

  drmModeRes* res;
  drmModeConnector* conn;

  mFd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (mFd < 0) {
    ALOGE("%s HWC2::Error opening DrmPresenter device: %d", __FUNCTION__,
          errno);
    return false;
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
    return false;
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
    return false;
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
  return true;
}

DrmPresenter::~DrmPresenter() { close(mFd); }

int DrmPresenter::setCrtc(hwc_drm_bo_t& bo) {
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

int DrmPresenter::getDrmFB(hwc_drm_bo_t& bo) {
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

int DrmPresenter::clearDrmFB(hwc_drm_bo_t& bo) {
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

bool DrmPresenter::supportComposeWithoutPost() { return true; }

int DrmPresenter::exportSyncFdAndSetCrtc(hwc_drm_bo_t& bo) {
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

DrmBuffer::DrmBuffer(const native_handle_t* handle, DrmPresenter& DrmPresenter)
    : mDrmPresenter(DrmPresenter), mBo({}) {
  if (!convertBoInfo(handle)) {
    mDrmPresenter.getDrmFB(mBo);
  }
}

DrmBuffer::~DrmBuffer() { mDrmPresenter.clearDrmFB(mBo); }

int DrmBuffer::convertBoInfo(const native_handle_t* handle) {
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

int DrmBuffer::flush() { return mDrmPresenter.exportSyncFdAndSetCrtc(mBo); }

}  // namespace android