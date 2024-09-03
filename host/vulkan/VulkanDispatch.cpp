// Copyright (C) 2018 The Android Open Source Project
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

#include "VulkanDispatch.h"

#include "aemu/base/SharedLibrary.h"
#include "aemu/base/files/PathUtils.h"
#include "aemu/base/synchronization/Lock.h"
#include "aemu/base/system/System.h"
#include "host-common/misc.h"

using android::base::AutoLock;
using android::base::Lock;
using android::base::pj;

#ifndef VERBOSE
#define VERBOSE INFO
#endif

namespace gfxstream {
namespace vk {

static std::string icdJsonNameToProgramAndLauncherPaths(const std::string& icdFilename) {
    std::string suffix = pj({"lib64", "vulkan", icdFilename});
#if defined(_WIN32)
    const char* sep = ";";
#else
    const char* sep = ":";
#endif
    return pj({android::base::getProgramDirectory(), suffix}) + sep +
           pj({android::base::getLauncherDirectory(), suffix});
}

static void setIcdPaths(const std::string& icdFilename) {
    const std::string paths = icdJsonNameToProgramAndLauncherPaths(icdFilename);
    INFO("Setting ICD filenames for the loader = %s", paths.c_str());
    android::base::setEnvironmentVariable("VK_ICD_FILENAMES", paths);
}

static const char* getTestIcdFilename() {
#if defined(__APPLE__)
    return "libvk_swiftshader.dylib";
#elif defined(__linux__) || defined(__QNX__)
    return "libvk_swiftshader.so";
#elif defined(_WIN32) || defined(_MSC_VER)
    return "vk_swiftshader.dll";
#else
#error Host operating system not supported
#endif
}

static void initIcdPaths(bool forTesting) {
    auto androidIcd = android::base::getEnvironmentVariable("ANDROID_EMU_VK_ICD");
    if (androidIcd == "") {
        // Rely on user to set VK_ICD_FILENAMES
        return;
    }

    if (forTesting) {
        const char* testingICD = "swiftshader";
        INFO("%s: In test environment, enforcing %s ICD.", __func__, testingICD);
        android::base::setEnvironmentVariable("ANDROID_EMU_VK_ICD", testingICD);
        androidIcd = testingICD;
    }

    if (androidIcd == "swiftshader") {
        INFO("%s: ICD set to 'swiftshader', using Swiftshader ICD", __func__);
        setIcdPaths("vk_swiftshader_icd.json");
    } else {
#ifdef __APPLE__
        // Mac: Use MoltenVK by default unless GPU mode is set to swiftshader
        if (androidIcd != "moltenvk") {
            WARN("%s: Unknown ICD, resetting to MoltenVK", __func__);
            android::base::setEnvironmentVariable("ANDROID_EMU_VK_ICD", "moltenvk");
        }
        setIcdPaths("MoltenVK_icd.json");

        // Configure MoltenVK library with environment variables
        // 0: No logging.
        // 1: Log errors only.
        // 2: Log errors and warning messages.
        // 3: Log errors, warnings and informational messages.
        // 4: Log errors, warnings, infos and debug messages.
        const bool verboseLogs =
            (android::base::getEnvironmentVariable("ANDROID_EMUGL_VERBOSE") == "1");
        const char* logLevelValue = verboseLogs ? "4" : "1";
        android::base::setEnvironmentVariable("MVK_CONFIG_LOG_LEVEL", logLevelValue);

        //  Limit MoltenVK to use single queue, as some older ANGLE versions
        //  expect this for -guest-angle to work.
        //  0: Limit Vulkan to a single queue, with no explicit semaphore
        //  synchronization, and use Metal's implicit guarantees that all operations
        //  submitted to a queue will give the same result as if they had been run in
        //  submission order.
        android::base::setEnvironmentVariable("MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE", "0");

        // TODO(b/364055067)
        // MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS is not working correctly
        android::base::setEnvironmentVariable("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0");

        // TODO(b/351765838): VVL won't work with MoltenVK due to the current
        //  way of external memory handling, add it into disable list to
        //  avoid users enabling it implicitly (i.e. via vkconfig).
        //  It can be enabled with VK_LOADER_LAYERS_ALLOW=VK_LAYER_KHRONOS_validation
        INFO("Vulkan Validation Layers won't be enabled with MoltenVK");
        android::base::setEnvironmentVariable("VK_LOADER_LAYERS_DISABLE",
                                              "VK_LAYER_KHRONOS_validation");
#else
        // By default, on other platforms, just use whatever the system
        // is packing.
#endif
    }
}

class SharedLibraries {
   public:
    explicit SharedLibraries(size_t sizeLimit = 1) : mSizeLimit(sizeLimit) {}

    size_t size() const { return mLibs.size(); }

    bool addLibrary(const std::string& path) {
        if (size() >= mSizeLimit) {
            WARN("Cannot add library %s due to size limit(%d)", path.c_str(), mSizeLimit);
            return false;
        }

        auto library = android::base::SharedLibrary::open(path.c_str());
        if (library) {
            mLibs.push_back(library);
            INFO("Added library: %s", path.c_str());
            return true;
        } else {
            // This is expected when searching for a valid library path
            VERBOSE("Library cannot be added: %s", path.c_str());
            return false;
        }
    }

    bool addFirstAvailableLibrary(const std::vector<std::string>& possiblePaths) {
        for (const std::string& possiblePath : possiblePaths) {
            if (addLibrary(possiblePath)) {
                return true;
            }
        }
        return false;
    }

    ~SharedLibraries() = default;

    void* dlsym(const char* name) {
        for (const auto& lib : mLibs) {
            void* funcPtr = reinterpret_cast<void*>(lib->findSymbol(name));
            if (funcPtr) {
                return funcPtr;
            }
        }
        return nullptr;
    }

   private:
    size_t mSizeLimit;
    std::vector<android::base::SharedLibrary*> mLibs;
};

static constexpr size_t getVulkanLibraryNumLimits() {
    return 1;
}

class VulkanDispatchImpl {
   public:
    VulkanDispatchImpl() : mVulkanLibs(getVulkanLibraryNumLimits()) {}

    void initialize(bool forTesting);

    static std::vector<std::string> getPossibleLoaderPathBasenames() {
#if defined(__APPLE__)
        return std::vector<std::string>{"libvulkan.dylib"};
#elif defined(__linux__)
        // When running applications with Gfxstream as the Vulkan ICD, i.e. with
        //
        //   App -> Vulkan Loader -> Gfxstream ICD -> Vulkan Loader -> Driver ICD
        //
        // Gfxstream needs to use a different nested loader library to avoid issues with
        // conflating/deadlock with the first level loader. Detect that here and look for
        // a "libvulkan_gfxstream" which can be generated with the provided
        // scripts/build-nested-vulkan-loader.sh script.
        const std::vector<std::string> nestedVulkanLoaderVars = {
            "GFXSTREAM_VK_ADD_DRIVER_FILES",
            "GFXSTREAM_VK_ADD_LAYER_PATH",
            "GFXSTREAM_VK_DRIVER_FILES",
            "GFXSTREAM_VK_ICD_FILENAMES",
            "GFXSTREAM_VK_INSTANCE_LAYERS",
            "GFXSTREAM_VK_LAYER_PATH",
            "GFXSTREAM_VK_LAYER_PATH",
            "GFXSTREAM_VK_LOADER_DEBUG",
            "GFXSTREAM_VK_LOADER_DRIVERS_DISABLE",
            "GFXSTREAM_VK_LOADER_DRIVERS_SELECT",
            "GFXSTREAM_VK_LOADER_LAYERS_ALLOW",
            "GFXSTREAM_VK_LOADER_LAYERS_DISABLE",
            "GFXSTREAM_VK_LOADER_LAYERS_ENABLE",
        };
        bool usesNestedVulkanLoader = false;
        for (const std::string& var : nestedVulkanLoaderVars) {
            if (android::base::getEnvironmentVariable(var) != "") {
                usesNestedVulkanLoader = true;
                break;
            }
        }
        if (usesNestedVulkanLoader) {
            return std::vector<std::string>{
                "libvulkan_gfxstream.so",
                "libvulkan_gfxstream.so.1",
            };
        } else {
            return std::vector<std::string>{
                "libvulkan.so",
                "libvulkan.so.1",
            };
        }

#elif defined(_WIN32)
        return std::vector<std::string>{"vulkan-1.dll"};
#elif defined(__QNX__)
        return std::vector<std::string>{
            "libvulkan.so",
            "libvulkan.so.1",
        };
#else
#error "Unhandled platform in VulkanDispatchImpl."
#endif
    }

    std::vector<std::string> getPossibleLoaderPaths() {
        const std::string explicitPath =
            android::base::getEnvironmentVariable("ANDROID_EMU_VK_LOADER_PATH");
        if (!explicitPath.empty()) {
            return {
                explicitPath,
            };
        }

        const std::vector<std::string> possibleBasenames = getPossibleLoaderPathBasenames();

        const std::string explicitIcd = android::base::getEnvironmentVariable("ANDROID_EMU_VK_ICD");

#ifdef _WIN32
        constexpr const bool isWindows = true;
#else
        constexpr const bool isWindows = false;
#endif
        if (explicitIcd.empty() || isWindows) {
            return possibleBasenames;
        }

        std::vector<std::string> possibleDirectories;

        if (mForTesting || explicitIcd == "mock") {
            possibleDirectories = {
                pj({android::base::getProgramDirectory(), "testlib64"}),
                pj({android::base::getLauncherDirectory(), "testlib64"}),
            };
        }

        possibleDirectories.push_back(
            pj({android::base::getProgramDirectory(), "lib64", "vulkan"}));
        possibleDirectories.push_back(
            pj({android::base::getLauncherDirectory(), "lib64", "vulkan"}));

        std::vector<std::string> possiblePaths;
        for (const std::string& possibleDirectory : possibleDirectories) {
            for (const std::string& possibleBasename : possibleBasenames) {
                possiblePaths.push_back(pj({possibleDirectory, possibleBasename}));
            }
        }
        return possiblePaths;
    }

    void* dlopen() {
        if (mVulkanLibs.size() == 0) {
            const std::vector<std::string> possiblePaths = getPossibleLoaderPaths();
            if (!mVulkanLibs.addFirstAvailableLibrary(possiblePaths)) {
                ERR("Cannot add any library for Vulkan loader from the list of %d items",
                    possiblePaths.size());
            }
        }
        return static_cast<void*>(&mVulkanLibs);
    }

    void* dlsym(void* lib, const char* name) {
        return (void*)((SharedLibraries*)(lib))->dlsym(name);
    }

    VulkanDispatch* dispatch() { return &mDispatch; }

   private:
    Lock mLock;
    bool mForTesting = false;
    bool mInitialized = false;
    VulkanDispatch mDispatch;
    SharedLibraries mVulkanLibs;
};

VulkanDispatchImpl* sVulkanDispatchImpl() {
    static VulkanDispatchImpl* impl = new VulkanDispatchImpl;
    return impl;
}

static void* sVulkanDispatchDlOpen() { return sVulkanDispatchImpl()->dlopen(); }

static void* sVulkanDispatchDlSym(void* lib, const char* sym) {
    return sVulkanDispatchImpl()->dlsym(lib, sym);
}

void VulkanDispatchImpl::initialize(bool forTesting) {
    AutoLock lock(mLock);

    if (mInitialized) {
        return;
    }

    mForTesting = forTesting;
    initIcdPaths(mForTesting);

    init_vulkan_dispatch_from_system_loader(sVulkanDispatchDlOpen, sVulkanDispatchDlSym,
                                            &mDispatch);

    mInitialized = true;
}

VulkanDispatch* vkDispatch(bool forTesting) {
    sVulkanDispatchImpl()->initialize(forTesting);
    return sVulkanDispatchImpl()->dispatch();
}

bool vkDispatchValid(const VulkanDispatch* vk) {
    return vk->vkEnumerateInstanceExtensionProperties != nullptr ||
           vk->vkGetInstanceProcAddr != nullptr || vk->vkGetDeviceProcAddr != nullptr;
}

}  // namespace vk
}  // namespace gfxstream
