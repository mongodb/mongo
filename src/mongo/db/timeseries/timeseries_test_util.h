// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/server_feature_flags_gen.h"

namespace mongo::timeseries::test_util {

/**
 * Returns the namespace where a timeseries collection actually lives.
 * For viewless timeseries, this is the main namespace.
 * For legacy timeseries, this is the system.buckets.* namespace.
 * (Ignore FCV check) Only used in unit tests.
 *
 * TODO SERVER-123350: Remove this once 9.0 is last LTS. Replace all call sites with just the nss.
 */
inline NamespaceString resolveTimeseriesNss(const NamespaceString& nss) {
    return gFeatureFlagCreateViewlessTimeseriesCollections.isEnabledAndIgnoreFCVUnsafe()
        ? nss
        : nss.makeTimeseriesBucketsNamespace();
}

}  // namespace mongo::timeseries::test_util
