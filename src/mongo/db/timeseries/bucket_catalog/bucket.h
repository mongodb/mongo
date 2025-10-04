/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/bucket_catalog/measurement_map.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_catalog/tracking_contexts.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"
#include "mongo/util/tracking/list.h"
#include "mongo/util/tracking/set.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>

#include <boost/container/small_vector.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::timeseries::bucket_catalog {

/**
 * The in-memory representation of a time-series bucket document. Maintains all the information
 * needed to add additional measurements, but does not generally store the full contents of the
 * document that have already been committed to disk.
 *
 * The order of members of this struct have been optimized for memory alignment, and therefore
 * a low memory footprint. Take extra care if modifying the order or adding or removing fields.
 */
struct Bucket {
private:
    // Number of new field names we can hold in NewFieldNames without needing to allocate memory.
    static constexpr std::size_t kNumStaticNewFields = 10;

public:
    // Before we hit our bucket minimum count, we will allow for large measurements to be inserted
    // into buckets. Instead of packing the bucket to the BSON size limit, 16MB, we'll limit the max
    // bucket size to 12MB. This is to leave some space in the bucket if we need to add new internal
    // fields to existing, full buckets.
    static constexpr int32_t kLargeMeasurementsMaxBucketSize =
        BSONObjMaxUserSize - (4 * 1024 * 1024);

    using NewFieldNames = boost::container::small_vector<StringMapHashedKey, kNumStaticNewFields>;

    Bucket(TrackingContexts&,
           const BucketId& bucketId,
           BucketKey bucketKey,
           StringData timeField,
           Date_t minTime,
           BucketStateRegistry& bucketStateRegistry);

    ~Bucket();

    Bucket(const Bucket&) = delete;
    Bucket(Bucket&&) = delete;
    Bucket& operator=(const Bucket&) = delete;
    Bucket& operator=(Bucket&&) = delete;

    // Whether the measurements in the bucket are sorted by timestamp or not.
    // True by default, if a v2 buckets gets promoted to v3 this is set to false.
    // It should not be used for v1 buckets.
    bool bucketIsSortedByTime = true;

    // Whether this bucket was kept open after exceeding the bucket max size to improve
    // bucketing performance for large measurements.
    bool keptOpenDueToLargeMeasurements = false;

    // Whether this bucket has a measurement that crossed the large measurement threshold. When this
    // threshold is crossed, we use the uncompressed size towards the bucket size limit for all
    // incoming measurements.
    bool crossedLargeMeasurementThreshold = false;

    // True if the bucket already exists and was reopened.
    bool isReopened = false;

    // The total compressed size in bytes of the bucket's BSON serialization,
    // not including measurements to be inserted until a WriteBatch is committed.
    int32_t size = 0;

    // The total number of measurements in the bucket, including uncommitted measurements and
    // measurements to be inserted.
    uint32_t numMeasurements = 0;

    // The number of committed measurements in the bucket.
    uint32_t numCommittedMeasurements = 0;

    // Whether the bucket has been marked for a rollover reason. It can be marked for closure
    // due to number of measurements, size, or schema changes, or it can be marked for archival
    // due to time range.
    RolloverReason rolloverReason = RolloverReason::kNone;

    // Minimum timestamp over contained measurements.
    const Date_t minTime;

    // The last era in which this bucket checked whether it was cleared.
    BucketStateRegistry::Era lastChecked;

    // A reference so we can clean up some linked state from the destructor.
    BucketStateRegistry& bucketStateRegistry;

    // The batch that has been prepared and is currently in the process of being committed, if
    // any.
    std::shared_ptr<WriteBatch> preparedBatch;

    // If the bucket is in idleBuckets, then its position is recorded here.
    using IdleList = tracking::list<Bucket*>;
    boost::optional<IdleList::iterator> idleListEntry = boost::none;

    // The bucket ID for the underlying document
    const BucketId bucketId;

    // Time field for the measurements that have been inserted into the bucket.
    const tracking::string timeField;

    // The key (i.e. (namespace, metadata)) for this bucket.
    const BucketKey key;

    // Top-level hashed field names of the measurements that have been inserted into the bucket.
    // These must be kept, in addition to the copies in measurementMap, so
    // we can still access them while std::move()ing measurementMap.
    tracking::StringSet fieldNames;

    // Top-level hashed new field names that have not yet been committed into the bucket.
    tracking::StringSet uncommittedFieldNames;

    // Batches, per operation, that haven't been committed or aborted yet.
    tracking::unordered_map<OperationId, std::shared_ptr<WriteBatch>> batches;

    // The minimum and maximum values for each field in the bucket.
    MinMax minmax;

    // The reference schema for measurements in this bucket. May reflect schema of uncommitted
    // measurements.
    Schema schema;

    /**
     * In-memory state of each committed data field. Enables fewer complete round-trips of
     * decompression + compression.
     */
    MeasurementMap measurementMap;
};

/**
 * Returns whether all measurements have been committed.
 */
bool allCommitted(const Bucket&);

/**
 * Determines if the schema for an incoming measurement is incompatible with those already
 * stored in the bucket.
 *
 * Returns true if incompatible
 */
bool schemaIncompatible(Bucket& bucket,
                        const BSONObj& input,
                        boost::optional<StringData> metaField,
                        const StringDataComparator* comparator);

/**
 * Determines the effect of adding 'doc' to this bucket. If adding 'doc' causes this bucket
 * to overflow, we will create a new bucket and recalculate the change to the bucket size
 * and data fields.
 *
 * It is impossible to know how well a measurement will compress in the
 * existing bucket ahead of time. We skip adding the element size to the calculation. The cost of
 * adding one more measurement over the limit won't be much, especially as it will get compressed on
 * commit. After committing, the Bucket is updated with the compressed size.
 */
void calculateBucketFieldsAndSizeChange(TrackingContexts&,
                                        const Bucket& bucket,
                                        const BSONObj& doc,
                                        boost::optional<StringData> metaField,
                                        Bucket::NewFieldNames& newFieldNamesToBeInserted,
                                        Sizes& sizesToBeAdded);

/**
 * Return a pointer to the current, open batch for the operation. Opens a new batch if none exists.
 */
std::shared_ptr<WriteBatch> activeBatch(TrackingContexts& trackingContexts,
                                        Bucket& bucket,
                                        OperationId opId,
                                        std::uint8_t stripe,
                                        ExecutionStatsController& stats);

}  // namespace mongo::timeseries::bucket_catalog
