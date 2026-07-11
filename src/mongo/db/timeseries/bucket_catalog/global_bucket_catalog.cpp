// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"

#include <cstddef>

namespace mongo::timeseries::bucket_catalog {
namespace {
const auto getGlobalBucketCatalog = ServiceContext::declareDecoration<GlobalBucketCatalog>();
static constexpr std::size_t kNumStripes = 32;
}  // namespace

GlobalBucketCatalog& GlobalBucketCatalog::get(ServiceContext* svcCtx) {
    return getGlobalBucketCatalog(svcCtx);
}

GlobalBucketCatalog::GlobalBucketCatalog()
    : BucketCatalog(kNumStripes, getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes) {}

}  // namespace mongo::timeseries::bucket_catalog
