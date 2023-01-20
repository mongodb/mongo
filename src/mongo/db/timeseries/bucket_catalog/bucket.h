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

#include <boost/container/small_vector.hpp>

#include "mongo/bson/oid.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/future.h"

namespace mongo::timeseries::bucket_catalog {

class BucketStateManager;

/**
 * The in-memory representation of a time-series bucket document. Maintains all the information
 * needed to add additional measurements, but does not generally store the full contents of the
 * document that have already been committed to disk.
 */
class Bucket {
    using StripeNumber = std::uint8_t;

public:
    friend class BucketCatalog;
    friend class WriteBatch;

    Bucket(const BucketId& bucketId,
           StripeNumber stripe,
           BucketKey::Hash keyHash,
           BucketStateManager* bucketStateManager);

    ~Bucket();

    uint64_t getEra() const;

    void setEra(uint64_t era);

    /**
     * Returns the BucketId for the bucket.
     */

    const BucketId& bucketId() const;

    /**
     * Returns the OID for the underlying bucket.
     */
    const OID& oid() const;

    /**
     * Returns the namespace for the underlying bucket.
     */
    const NamespaceString& ns() const;

    /**
     * Returns the number of the stripe that owns the bucket.
     */
    StripeNumber stripe() const;

    /**
     * Returns the pre-computed hash of the corresponding BucketKey.
     */
    BucketKey::Hash keyHash() const;

    /**
     * Returns the time associated with the bucket (id).
     */
    Date_t getTime() const;

    /**
     * Returns the timefield for the underlying bucket.
     */
    StringData getTimeField();

    /**
     * Returns whether all measurements have been committed.
     */
    bool allCommitted() const;

    /**
     * Returns total number of measurements in the bucket.
     */
    uint32_t numMeasurements() const;

    /**
     * Sets the rollover action, to determine what to do with a bucket when all measurements
     * have been committed.
     */
    void setRolloverAction(RolloverAction action);

    /**
     * Determines if the schema for an incoming measurement is incompatible with those already
     * stored in the bucket.
     *
     * Returns true if incompatible
     */
    bool schemaIncompatible(const BSONObj& input,
                            boost::optional<StringData> metaField,
                            const StringData::ComparatorInterface* comparator);

private:
    /**
     * Determines the effect of adding 'doc' to this bucket. If adding 'doc' causes this bucket
     * to overflow, we will create a new bucket and recalculate the change to the bucket size
     * and data fields.
     */
    void _calculateBucketFieldsAndSizeChange(const BSONObj& doc,
                                             boost::optional<StringData> metaField,
                                             NewFieldNames* newFieldNamesToBeInserted,
                                             int32_t* sizeToBeAdded) const;

    /**
     * Returns whether BucketCatalog::commit has been called at least once on this bucket.
     */
    bool _hasBeenCommitted() const;

    /**
     * Return a pointer to the current, open batch.
     */
    std::shared_ptr<WriteBatch> _activeBatch(OperationId opId, ExecutionStatsController& stats);

protected:
    // The era number of the last log operation the bucket has caught up to
    uint64_t _lastCheckedEra;

    BucketStateManager* _bucketStateManager;

private:
    // The bucket ID for the underlying document
    const BucketId _bucketId;

    // The stripe which owns this bucket.
    const StripeNumber _stripe;

    // The pre-computed hash of the associated BucketKey
    const BucketKey::Hash _keyHash;

    // The metadata of the data that this bucket contains.
    BucketMetadata _metadata;

    // Top-level hashed field names of the measurements that have been inserted into the bucket.
    StringSet _fieldNames;

    // Top-level hashed new field names that have not yet been committed into the bucket.
    StringSet _uncommittedFieldNames;

    // Time field for the measurements that have been inserted into the bucket.
    std::string _timeField;

    // Minimum timestamp over contained measurements
    Date_t _minTime;

    // The minimum and maximum values for each field in the bucket.
    MinMax _minmax;

    // The reference schema for measurements in this bucket. May reflect schema of uncommitted
    // measurements.
    Schema _schema;

    // The total size in bytes of the bucket's BSON serialization, including measurements to be
    // inserted.
    int32_t _size = 0;

    // The total number of measurements in the bucket, including uncommitted measurements and
    // measurements to be inserted.
    uint32_t _numMeasurements = 0;

    // The number of committed measurements in the bucket.
    uint32_t _numCommittedMeasurements = 0;

    // Whether the bucket has been marked for a rollover action. It can be marked for closure
    // due to number of measurements, size, or schema changes, or it can be marked for archival
    // due to time range.
    RolloverAction _rolloverAction = RolloverAction::kNone;

    // Whether this bucket was kept open after exceeding the bucket max size to improve
    // bucketing performance for large measurements.
    bool _keptOpenDueToLargeMeasurements = false;

    // The batch that has been prepared and is currently in the process of being committed, if
    // any.
    std::shared_ptr<WriteBatch> _preparedBatch;

    // Batches, per operation, that haven't been committed or aborted yet.
    stdx::unordered_map<OperationId, std::shared_ptr<WriteBatch>> _batches;

    // If the bucket is in idleBuckets, then its position is recorded here.
    using IdleList = std::list<Bucket*>;
    boost::optional<IdleList::iterator> _idleListEntry = boost::none;

    // Approximate memory usage of this bucket.
    uint64_t _memoryUsage = sizeof(*this);

    // If set, bucket is compressed on disk, and first prepared batch will need to decompress it
    // before updating.
    boost::optional<DecompressionResult> _decompressed;
};

}  // namespace mongo::timeseries::bucket_catalog
