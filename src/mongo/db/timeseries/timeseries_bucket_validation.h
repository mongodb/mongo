// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/timeseries/timeseries_options.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries {
/**
 * Performs strict validation of a timeseries bucket document.
 * Throws an exception on any validation failure
 *
 * The strict validation validates the following properties:
 * - Bucket schema adheres to specification depending on bucket version.
 * - Control.min.time is identical to bucket _id time for dates within normal range
 * - Bucket time span is compatible with collection granularity/max span setting.
 * - Rounding of control.min.time for collections with fixed-bucket optimization enabled.
 * - Control.count matches count of all data columns
 * - Control.min is equal to min of data column content.
 * - Control.max is equal to max of data column content.
 *
 * Some properties that are NOT validated due to existing data and benign impact:
 * - Presence of mixed schema
 * - Rounding of control.min.time, for collections without fixed-bucket optimization.
 * - Equality of control.max.time, for buckets with extended range dates.
 * - Min/max time for dates outside of normal range.
 * - v3 bucket sortedness
 */
void validateBucketConsistency(const Collection* collection, const BSONObj& bucketDoc);

/**
 * TODO SERVER-122862: Use in validation command
 */
void validateBucketIdTimestamp(const TimeseriesOptions& timeseriesOptions,
                               const OID& id,
                               const BSONObj& controlMin,
                               bool criticalValidationOnly);

/**
 * TODO SERVER-122862: Use in validation command
 */
void validateBucketTimeSpan(const TimeseriesOptions& timeseriesOptions,
                            const BSONObj& controlMin,
                            const BSONObj& controlMax,
                            bool criticalValidationOnly);

/**
 * TODO SERVER-122862: Use in validation command
 */
void validateBucketData(const TimeseriesOptions& timeseriesOptions,
                        const CollatorInterface* collator,
                        int bucketVersion,
                        BSONElement controlCount,
                        const BSONObj& controlMin,
                        const BSONObj& controlMax,
                        const BSONObj& data,
                        bool criticalValidationOnly);
}  // namespace mongo::timeseries
