/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/str.h"

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
    } else {
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
