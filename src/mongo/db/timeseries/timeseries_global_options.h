// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

extern Atomic<long long> gTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes;
uint64_t getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes();

extern Atomic<long long> gTimeseriesSideBucketCatalogMemoryUsageThresholdBytes;
uint64_t getTimeseriesSideBucketCatalogMemoryUsageThresholdBytes();
/**
 * Checks the time or the meta field doesn't contain embedded null bytes.
 */
inline Status validateTimeAndMetaField(const std::string& str) {
    if (str.find('\0') != std::string::npos) {
        return Status(ErrorCodes::BadValue,
                      "The 'timeField' or the 'metaField' cannot contain embedded null bytes");
    }
    return Status::OK();
}

}  // namespace mongo
