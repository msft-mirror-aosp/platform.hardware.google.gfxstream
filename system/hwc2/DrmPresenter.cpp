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

  bool configRet = configDrmElements();
  if (configRet) {
    ALOGD("%s: Successfully initialized DRM backend", __FUNCTION__);
  } else {
    ALOGD("%s: Failed to initialize DRM backend", __FUNCTION__);
  }

  return configRet;
}

void DrmPresenter::clearDrmElements() {
  mConnectors.clear();
  mCrtcs.clear();
  mPlanes.clear();
}

bool DrmPresenter::configDrmElements() {
  drmModeRes* res;

  res = drmModeGetResources(mFd);
  if (res == nullptr) {
    ALOGE("%s HWC2::Error reading drm resources: %d", __FUNCTION__, errno);
    close(mFd);
    mFd = -1;
    return false;
  }

  ALOGI(
      "drmModeRes count fbs %d crtc %d connector %d encoder %d min w %d max w "
      "%d min h %d max h %d",
      res->count_fbs, res->count_crtcs, res->count_connectors,
      res->count_encoders, res->min_width, res->max_width, res->min_height,
      res->max_height);

  for (uint32_t i = 0; i < res->count_crtcs; i++) {
    DrmCrtc crtc = {};

    drmModeCrtcPtr c = drmModeGetCrtc(mFd, res->crtcs[i]);
    crtc.mId = c->crtc_id;

    drmModeObjectPropertiesPtr crtcProps =
        drmModeObjectGetProperties(mFd, c->crtc_id, DRM_MODE_OBJECT_CRTC);

    for (uint32_t crtcPropsIndex = 0; crtcPropsIndex < crtcProps->count_props;
         crtcPropsIndex++) {
      drmModePropertyPtr crtcProp =
          drmModeGetProperty(mFd, crtcProps->props[crtcPropsIndex]);

      if (!strcmp(crtcProp->name, "OUT_FENCE_PTR")) {
        crtc.mFencePropertyId = crtcProp->prop_id;
        ALOGI("%s: Crtc %" PRIu32 " fence property id: %" PRIu32, __FUNCTION__,
              crtc.mId, crtcProp->prop_id);
      } else if (!strcmp(crtcProp->name, "ACTIVE")) {
        crtc.mActivePropertyId = crtcProp->prop_id;
        ALOGI("%s: Crtc %" PRIu32 " active property id: %" PRIu32, __FUNCTION__,
              crtc.mId, crtcProp->prop_id);
      } else if (!strcmp(crtcProp->name, "MODE_ID")) {
        crtc.mModePropertyId = crtcProp->prop_id;
        ALOGI("%s: Crtc %" PRIu32 " mode property id: %" PRIu32, __FUNCTION__,
              crtc.mId, crtcProp->prop_id);
      }

      drmModeFreeProperty(crtcProp);
    }

    drmModeFreeObjectProperties(crtcProps);

    mCrtcs.push_back(crtc);
  }

  drmModePlaneResPtr planeRes = drmModeGetPlaneResources(mFd);
  for (uint32_t i = 0; i < planeRes->count_planes; ++i) {
    DrmPlane plane = {};

    drmModePlanePtr p = drmModeGetPlane(mFd, planeRes->planes[i]);
    plane.mId = p->plane_id;

    ALOGI(
        "%s: plane id: %u crtcid %u fbid %u crtc xy %d %d xy %d %d "
        "possible ctrcs 0x%x",
        __FUNCTION__, p->plane_id, p->crtc_id, p->fb_id, p->crtc_x, p->crtc_y,
        p->x, p->y, p->possible_crtcs);

    drmModeObjectPropertiesPtr planeProps =
        drmModeObjectGetProperties(mFd, plane.mId, DRM_MODE_OBJECT_PLANE);

    for (uint32_t planePropIndex = 0; planePropIndex < planeProps->count_props;
         ++planePropIndex) {
      drmModePropertyPtr planeProp =
          drmModeGetProperty(mFd, planeProps->props[planePropIndex]);

      if (!strcmp(planeProp->name, "CRTC_ID")) {
        plane.mCrtcPropertyId = planeProp->prop_id;
        ALOGD("%s: plane %" PRIu32 " crtc property %u", __FUNCTION__, plane.mId,
              planeProp->prop_id);
      } else if (!strcmp(planeProp->name, "FB_ID")) {
        plane.mFbPropertyId = planeProp->prop_id;
        ALOGD("%s: plane %" PRIu32 " fb property id %" PRIu32, __FUNCTION__,
              plane.mId, planeProp->prop_id);
      } else if (!strcmp(planeProp->name, "CRTC_X")) {
        plane.mCrtcXPropertyId = planeProp->prop_id;
        ALOGD("%s: plane %" PRIu32 " crtc X property id %" PRIu32, __FUNCTION__,
              plane.mId, planeProp->prop_id);
      } else if (!strcmp(planeProp->name, "CRTC_Y")) {
        plane.mCrtcYPropertyId = planeProp->prop_id;
        ALOGD("%s: plane %" PRIu32 " crtc Y property id %" PRIu32, __FUNCTION__,
              plane.mId, planeProp->prop_id);
      } else if (!strcmp(planeProp->name, "CRTC_W")) {
        plane.mCrtcWPropertyId = planeProp->prop_id;
        ALOGD("%s: plane %" PRIu32 " crtc W property id %" PRIu32, __FUNCTION__,
              plane.mId, planeProp->prop_id);
      } else if (!strcmp(planeProp->name, "CRTC_H")) {
        plane.mCrtcHPropertyId = planeProp->prop_id;
        ALOGD("%s: plane %" PRIu32 " crtc H property id %" PRIu32, __FUNCTION__,
              plane.mId, planeProp->prop_id);
      } else if (!strcmp(planeProp->name, "SRC_X")) {
        plane.mSrcXPropertyId = planeProp->prop_id;
        ALOGD("%s: plane %" PRIu32 " src X property id %" PRIu32, __FUNCTION__,
              plane.mId, planeProp->prop_id);
      } else if (!strcmp(planeProp->name, "SRC_Y")) {
        plane.mSrcYPropertyId = planeProp->prop_id;
        ALOGD("%s: plane %" PRIu32 " src Y property id %" PRIu32, __FUNCTION__,
              plane.mId, planeProp->prop_id);
      } else if (!strcmp(planeProp->name, "SRC_W")) {
        plane.mSrcWPropertyId = planeProp->prop_id;
        ALOGD("%s: plane %" PRIu32 " src W property id %" PRIu32, __FUNCTION__,
              plane.mId, planeProp->prop_id);
      } else if (!strcmp(planeProp->name, "SRC_H")) {
        plane.mSrcHPropertyId = planeProp->prop_id;
        ALOGD("%s: plane %" PRIu32 " src H property id %" PRIu32, __FUNCTION__,
              plane.mId, planeProp->prop_id);
      } else if (!strcmp(planeProp->name, "type")) {
        plane.mTypePropertyId = planeProp->prop_id;
        ALOGD("%s: plane %" PRIu32 " type property id %" PRIu32, __FUNCTION__,
              plane.mId, planeProp->prop_id);
        uint64_t type = planeProp->values[0];
        switch (type) {
          case DRM_PLANE_TYPE_OVERLAY:
            plane.mType = type;
            ALOGD("%s: plane %" PRIu32 " is DRM_PLANE_TYPE_OVERLAY",
                  __FUNCTION__, plane.mId);
            break;
          case DRM_PLANE_TYPE_PRIMARY:
            plane.mType = type;
            ALOGD("%s: plane %" PRIu32 " is DRM_PLANE_TYPE_PRIMARY",
                  __FUNCTION__, plane.mId);
            break;
          default:
            break;
        }
      }

      drmModeFreeProperty(planeProp);
    }

    drmModeFreeObjectProperties(planeProps);

    bool isPrimaryOrOverlay = plane.mType == DRM_PLANE_TYPE_OVERLAY ||
                              plane.mType == DRM_PLANE_TYPE_PRIMARY;
    if (isPrimaryOrOverlay && ((1 << 0) & p->possible_crtcs)) {
      ALOGD("%s: plane %" PRIu32 " compatible with crtc mask %" PRIu32,
            __FUNCTION__, plane.mId, p->possible_crtcs);
    }

    if (plane.mType == DRM_PLANE_TYPE_OVERLAY ||
        plane.mType == DRM_PLANE_TYPE_PRIMARY) {
      // TODO: correctly convert mask.
      uint32_t crtcIndex = p->possible_crtcs - 1;

      DrmCrtc& crtc = mCrtcs[crtcIndex];

      if (crtc.mPlaneId == -1) {
        crtc.mPlaneId = plane.mId;
        ALOGE("%s: plane %" PRIu32 " associated with crtc %" PRIu32,
              __FUNCTION__, plane.mId, crtc.mId);
      }
    }

    drmModeFreePlane(p);

    mPlanes[plane.mId] = plane;
  }
  drmModeFreePlaneResources(planeRes);

  for (uint32_t i = 0; i < res->count_connectors; ++i) {
    DrmConnector connector = {};
    connector.mId = res->connectors[i];

    {
      drmModeObjectPropertiesPtr connectorProps = drmModeObjectGetProperties(
          mFd, connector.mId, DRM_MODE_OBJECT_CONNECTOR);

      for (uint32_t connectorPropIndex = 0;
           connectorPropIndex < connectorProps->count_props;
           ++connectorPropIndex) {
        drmModePropertyPtr connectorProp =
            drmModeGetProperty(mFd, connectorProps->props[connectorPropIndex]);
        if (!strcmp(connectorProp->name, "CRTC_ID")) {
          connector.mCrtcPropertyId = connectorProp->prop_id;
          ALOGD("%s: connector %" PRIu32 " crtc prop id %" PRIu32, __FUNCTION__,
                connector.mId, connectorProp->prop_id);
        }
        drmModeFreeProperty(connectorProp);
      }

      drmModeFreeObjectProperties(connectorProps);
    }
    {
      drmModeConnector* c = drmModeGetConnector(mFd, connector.mId);
      if (c == nullptr) {
        ALOGE("%s: Failed to get connector %" PRIu32 ": %d", __FUNCTION__,
              connector.mId, errno);
        return false;
      }

      memcpy(&connector.mMode, &c->modes[0], sizeof(drmModeModeInfo));

      drmModeFreeConnector(c);

      drmModeCreatePropertyBlob(mFd, &connector.mMode, sizeof(connector.mMode),
                                &connector.mModeBlobId);

      connector.mRefreshRateAsFloat =
          1000.0f * connector.mMode.clock /
          ((float)connector.mMode.vtotal * (float)connector.mMode.htotal);
      connector.mRefreshRateAsInteger =
          (uint32_t)(connector.mRefreshRateAsFloat + 0.5f);
    }

    mConnectors.push_back(connector);
  }

  drmModeFreeResources(res);
  return true;
}

DrmPresenter::~DrmPresenter() { close(mFd); }

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

HWC2::Error DrmPresenter::flushToDisplay(int display, hwc_drm_bo_t& bo,
                                         int* outSyncFd) {
  DrmConnector& connector = mConnectors[display];
  DrmCrtc& crtc = mCrtcs[display];

  HWC2::Error error = HWC2::Error::None;

  *outSyncFd = -1;

  drmModeAtomicReqPtr pset = drmModeAtomicAlloc();

  int ret;

  if (!crtc.mDidSetCrtc) {
    DEBUG_LOG("%s: Setting crtc.\n", __FUNCTION__);
    ret = drmModeAtomicAddProperty(pset, crtc.mId, crtc.mActivePropertyId, 1);
    if (ret < 0) {
      ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
    }
    ret = drmModeAtomicAddProperty(pset, crtc.mId, crtc.mModePropertyId,
                                   connector.mModeBlobId);
    if (ret < 0) {
      ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
    }
    ret = drmModeAtomicAddProperty(pset, connector.mId,
                                   connector.mCrtcPropertyId, crtc.mId);
    if (ret < 0) {
      ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
    }

    crtc.mDidSetCrtc = true;
  } else {
    DEBUG_LOG("%s: Already set crtc\n", __FUNCTION__);
  }

  uint64_t outSyncFdUint = (uint64_t)outSyncFd;

  ret = drmModeAtomicAddProperty(pset, crtc.mId, crtc.mFencePropertyId,
                                 outSyncFdUint);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }

  if (crtc.mPlaneId == -1) {
    ALOGE("%s:%d: no plane available for crtc id %" PRIu32, __FUNCTION__,
          __LINE__, crtc.mId);
    return HWC2::Error::NoResources;
  }

  DrmPlane& plane = mPlanes[crtc.mPlaneId];

  DEBUG_LOG("%s: set plane: plane id %d crtc id %d fbid %d bo w h %d %d\n",
            __FUNCTION__, plane.mId, crtc.mId, bo.fb_id, bo.width, bo.height);

  ret = drmModeAtomicAddProperty(pset, plane.mId, plane.mCrtcPropertyId,
                                 crtc.mId);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret =
      drmModeAtomicAddProperty(pset, plane.mId, plane.mFbPropertyId, bo.fb_id);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, plane.mId, plane.mCrtcXPropertyId, 0);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, plane.mId, plane.mCrtcYPropertyId, 0);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, plane.mId, plane.mCrtcWPropertyId,
                                 bo.width);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, plane.mId, plane.mCrtcHPropertyId,
                                 bo.height);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, plane.mId, plane.mSrcXPropertyId, 0);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, plane.mId, plane.mSrcYPropertyId, 0);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, plane.mId, plane.mSrcWPropertyId,
                                 bo.width << 16);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }
  ret = drmModeAtomicAddProperty(pset, plane.mId, plane.mSrcHPropertyId,
                                 bo.height << 16);
  if (ret < 0) {
    ALOGE("%s:%d: failed %d errno %d\n", __FUNCTION__, __LINE__, ret, errno);
  }

  uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
  ret = drmModeAtomicCommit(mFd, pset, flags, 0);

  if (ret) {
    ALOGE("%s: Atomic commit failed with %d %d\n", __FUNCTION__, ret, errno);
    error = HWC2::Error::NoResources;
  }

  if (pset) {
    drmModeAtomicFree(pset);
  }

  DEBUG_LOG("%s: out fence: %d\n", __FUNCTION__, *outSyncFd);
  return error;
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

HWC2::Error DrmBuffer::flushToDisplay(int display, int* outFlushDoneSyncFd) {
  return mDrmPresenter.flushToDisplay(display, mBo, outFlushDoneSyncFd);
}

}  // namespace android
