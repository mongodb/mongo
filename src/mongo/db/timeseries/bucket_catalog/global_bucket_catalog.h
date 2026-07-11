// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];
namespace mongo::timeseries::bucket_catalog {

/**
 * The global bucket catalog with a fixed number of stripes, decorated on the service context.
 */
class GlobalBucketCatalog : public BucketCatalog {
public:
    static GlobalBucketCatalog& get(ServiceContext*);

    GlobalBucketCatalog();
};

}  // namespace mongo::timeseries::bucket_catalog
