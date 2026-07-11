// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries {

/**
 * Maps the time-series collection index spec 'timeseriesIndexSpecBSON' to the index schema of the
 * underlying bucket collection using the information provided in 'timeseriesOptions'.
 *
 * Returns an error if the specified 'timeseriesKeyBSON' is invalid for the time-series collection.
 */
StatusWith<BSONObj> createBucketsIndexSpecFromTimeseriesIndexSpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& timeseriesIndexSpecBSON);

StatusWith<BSONObj> createBucketsShardKeySpecFromTimeseriesShardKeySpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& timeseriesIndexSpecBSON);

boost::optional<BSONObj> createTimeseriesIndexFromBucketsIndexSpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& bucketsIndexSpecBSON);

/**
 * Maps a bucket collection shard key to a bucket collection index backing the shard key using the
 * information provided in 'timeseriesOptions'.
 *
 * Returns boost::none if the specified 'bucketShardKeySpecBSON' is invalid for the time-series
 * collection.
 */
boost::optional<BSONObj> createBucketsShardKeyIndexFromBucketsShardKeySpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& bucketShardKeySpecBSON);

/**
 * Returns a time-series collection index spec equivalent to the given 'bucketsIndex' using the
 * time-series specifications provided in 'timeseriesOptions'. Returns boost::none if the
 * buckets index is not supported on a time-series collection.
 *
 * Copies and modifies the 'key' field of the buckets index, but otherwise copies all of the fields
 * over unaltered.
 */
boost::optional<BSONObj> createTimeseriesIndexFromBucketsIndex(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& bucketsIndex);

/**
 * Returns true if the original index specification should be included when creating an index on the
 * time-series buckets collection.
 */
bool shouldIncludeOriginalSpec(const TimeseriesOptions& timeseriesOptions,
                               const BSONObj& bucketsIndex);

/**
 * Returns true if 'bucketsIndex' uses a measurement field, excluding the time field. Checks both
 * the index key and the partialFilterExpression, if present.
 *
 * This is helpful to detect if the 'bucketsIndex' relies on a field that was allowed to have
 * mixed-schema data in MongoDB versions < 5.2.
 */
bool doesBucketsIndexIncludeMeasurement(OperationContext* opCtx,
                                        const NamespaceString& bucketNs,
                                        const TimeseriesOptions& timeseriesOptions,
                                        const BSONObj& bucketsIndex);

/**
 * Takes a 'hint' object, in the same format used by FindCommandRequest, and returns
 * true if the hint is an index key.
 *
 * Besides an index key, a hint can be {$hint: <index name>} or {$natural: <direction>},
 * or it can be {} which means no hint is given.
 */
bool isHintIndexKey(const BSONObj& obj);

/**
 * Returns an index hint which we can use for query based reopening, if a suitable index exists for
 * the collection.
 */
boost::optional<BSONObj> getIndexSupportingReopeningQuery(OperationContext* opCtx,
                                                          const IndexCatalog* indexCatalog,
                                                          const TimeseriesOptions& tsOptions);

}  // namespace mongo::timeseries
