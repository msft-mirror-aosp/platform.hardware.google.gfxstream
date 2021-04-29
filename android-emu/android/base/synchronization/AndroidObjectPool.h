// Copyright 2021 The Android Open Source Project
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

#pragma once

#include "android/base/synchronization/AndroidConditionVariable.h"
#include "android/base/synchronization/AndroidLock.h"

#include <list>
#include <queue>

namespace android {
namespace base {
namespace guest {

template <class T>
class ObjectPool {
public:
    explicit ObjectPool(size_t sizeLimit,
                        std::function<T()> createObject,
                        std::function<void(T&)> onDestroy = {[] {}},
                        std::function<void(T&)> onRelease = {[] {}})
        : mCreateObject(std::move(createObject)),
          mOnDestroy(std::move(onDestroy)),
          mOnRelease(std::move(onRelease)),
          mSizeLimit(sizeLimit) {}

    virtual ~ObjectPool() {
        while (!mObjects.empty()) {
            mOnDestroy(mObjects.back());
            mObjects.pop_back();
        }
    };

    void setCreateObjectFunc(std::function<T()> createObject) {
        mCreateObject = std::move(createObject);
    }

    void setObjectOnDestroyCallback(std::function<void(T&)> onDestroy) {
        mOnDestroy = std::move(onDestroy);
    }

    void setObjectOnReleaseCallback(std::function<void(T&)> onRelease) {
        mOnRelease = std::move(onRelease);
    }

    T* acquire() {
        AutoLock lock(mLock);
        T* obj = nullptr;
        if (!mAvailableObjects.empty()) {
            obj = mAvailableObjects.front();
            mAvailableObjects.pop();
            return obj;
        }

        if (mSizeLimit == 0 || mObjects.size() < mSizeLimit) {
            mObjects.emplace_back(mCreateObject());
            return &mObjects.back();
        }

        mCv.wait(&lock, [this, &obj] {
            if (!mAvailableObjects.empty()) {
                obj = mAvailableObjects.front();
                mAvailableObjects.pop();
                return true;
            }
            return false;
        });
        return obj;
    }

    void release(T* obj) {
        mOnRelease(*obj);

        AutoLock lock(mLock);
        mAvailableObjects.push(obj);
        mCv.signalAndUnlock(&lock);
    }

private:
    // Called when ObjectPool creates a new Object.
    std::function<T()> mCreateObject;

    // Called when an Object is destroyed.
    // This function will be called in the base class destructor, thus any
    // method / member variables from derived class should NOT be used in
    // this function.
    std::function<void(T& obj)> mOnDestroy;

    // Called when an Object is released back to the pool.
    std::function<void(T& obj)> mOnRelease;

    android::base::guest::StaticLock mLock;
    android::base::guest::StaticLock mCvLock;
    android::base::guest::ConditionVariable mCv;
    std::queue<T*> mAvailableObjects;
    std::list<T> mObjects;
    size_t mTotalObjectsCreated = 0u;

    size_t mSizeLimit = 1u;
};

template <class T>
class DefaultObjectPool : public ObjectPool<T> {
public:
    explicit DefaultObjectPool(size_t sizeLimit)
        : ObjectPool<T>(
              sizeLimit,
              /* createObject */ [] { return T{}; },
              /* onDestroy */ [] {},
              /* onRelease */ [] {}) {}
    ~DefaultObjectPool() = default;

private:
    T createObject() { return T{}; }
    void destroyObject(T&) {}
};

}  // namespace guest
}  // namespace base
}  // namespace android
