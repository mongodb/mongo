// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/processinfo.h"

#include <cstdint>

namespace mongo {

Atomic<long long> gTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes{-1};
Atomic<long long> gTimeseriesSideBucketCatalogMemoryUsageThresholdBytes{104857600};  // 100MB

uint64_t getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes() {
    long long userValue = gTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes.load();
    if (userValue <= 0) {
        userValue =
            kTimeseriesIdleBucketExpiryMemoryUsageThresholdDefault;  // Non-positive values are
                                                                     // interpreted as default
                                                                     // percentage of system memory
    }

    if (userValue <= 100) {
        const uint64_t systemBasedValue{ProcessInfo::getSystemMemSizeMB() * userValue *
                                        10485};  // ~% of system memory. 10485 ~= 1024*1024/100.
        return systemBasedValue;
    }

    return userValue;
}

uint64_t getTimeseriesSideBucketCatalogMemoryUsageThresholdBytes() {
    return static_cast<uint64_t>(gTimeseriesSideBucketCatalogMemoryUsageThresholdBytes.load());
}

}  // namespace mongo
