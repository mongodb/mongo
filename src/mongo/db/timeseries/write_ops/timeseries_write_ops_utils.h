// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries::write_ops {

/**
 * Constructs an update request using a single update statement at position `opIndex`.
 */
mongo::write_ops::UpdateCommandRequest buildSingleUpdateOp(
    const mongo::write_ops::UpdateCommandRequest& wholeOp, size_t opIndex);

void assertTimeseriesBucketsCollectionNotFound(const mongo::NamespaceString& ns);

/**
 * Returns the document for writing a new bucket with 'measurements'. Generates the id and
 * calculates the min and max fields while building the document.
 *
 * The measurements must already be known to fit in the same bucket. No checks will be done.
 */
BSONObj makeBucketDocument(const std::vector<BSONObj>& measurements,
                           const NamespaceString& nss,
                           const UUID& collectionUUID,
                           const TimeseriesOptions& options,
                           const StringDataComparator* comparator);

}  // namespace mongo::timeseries::write_ops
