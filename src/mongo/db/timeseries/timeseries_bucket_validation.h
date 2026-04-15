/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/timeseries/timeseries_options.h"

MONGO_MOD_PUBLIC;

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
                            bool fixedBucketingEnabled,
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
