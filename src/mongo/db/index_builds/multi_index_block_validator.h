// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#include <cmath>

#include <boost/optional/optional.hpp>


namespace mongo {

constexpr const double kMinIndexBuildMemSizeLimitMB = 50.0;
constexpr const double kMinIndexBuildMemSizeLimitBytes = kMinIndexBuildMemSizeLimitMB * 1024 * 1024;
constexpr const double kMaxIndexBuildMemSizeLimitPercent = 0.8;


inline Status validateMaxIndexBuildMemoryUsageMegabytesSetting(const double& limit,
                                                               const boost::optional<TenantId>&) {
    if (std::isnan(limit)) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "maxIndexBuildMemoryUsageMegabytes must be a positive value");
    } else if (limit < 1 && limit > 0) {
        // Percentage-based value.
        if (limit > kMaxIndexBuildMemSizeLimitPercent) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "maxIndexBuildMemoryUsageMegabytes parsed as a "
                                           "percentage. Must be below max percentage limit of "
                                        << kMaxIndexBuildMemSizeLimitPercent * 100 << "%");
        }
    } else if (limit <= 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "maxIndexBuildMemoryUsageMegabytes must be a positive value");
    } else if (!TestingProctor::instance().isInitialized() ||
               !TestingProctor::instance().isEnabled()) {
        // Byte-based value.
        if (limit < kMinIndexBuildMemSizeLimitMB) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Must be above minimum of "
                                        << kMinIndexBuildMemSizeLimitMB << " MB");
        }
    }


    return Status::OK();
}

}  // namespace mongo
