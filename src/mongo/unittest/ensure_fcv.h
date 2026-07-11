// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/server_options.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace unittest {

/**
 * Helper class for tests to set FCV to their desired version. Will unset
 * FCV when its destructor runs.
 */
class EnsureFCV {
public:
    using Version = multiversion::FeatureCompatibilityVersion;
    EnsureFCV(Version version)
        : _origVersion(serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion()) {
        serverGlobalParams.mutableFCV.setVersion(version);
    }
    ~EnsureFCV() {
        serverGlobalParams.mutableFCV.setVersion(_origVersion);
    }

private:
    const Version _origVersion;
};

}  // namespace unittest
}  // namespace mongo
