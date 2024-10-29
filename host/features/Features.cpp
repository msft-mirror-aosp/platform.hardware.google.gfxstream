// Copyright 2024 The Android Open Source Project
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

#include "gfxstream/host/Features.h"
#include <sstream>
#include <vector>
namespace gfxstream {
namespace host {

FeatureSet::FeatureSet(const FeatureSet& rhs) : FeatureSet() {
    *this = rhs;
}

FeatureResult FeatureDependencyHandler::checkAllDependentFeaturesAreEnabled() {
    // Only check for direct dependencies. Since we're verifying all enabled features, this should cover the whole span.
    bool allDependenciesAreEnabled = true;
    std::stringstream ss;
    for (auto const&[feature, dependentFeatures] : VK_FEATURE_DEPENDENCY_MAP) {
        if (feature->enabled && !dependentFeatures.empty()) {
            for (auto const& dep : dependentFeatures) {
                if (!dep->enabled) {
                    ss << "Feature: " << feature->name << " requests missing dependency: " << dep->name << "\n";
                    allDependenciesAreEnabled = false;
                }
            }
        }
    }
    return {allDependenciesAreEnabled, ss.str()};
};

FeatureSet& FeatureSet::operator=(const FeatureSet& rhs) {
    for (const auto& [featureName, featureInfo] : rhs.map) {
        *map[featureName] = *featureInfo;
    }
    return *this;
}

}  // host
}  // gfxstream
