// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/measurement_map.h"
#include "mongo/db/timeseries/bucket_catalog/tracking_contexts.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <boost/container/small_vector.hpp>
#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::timeseries::bucket_catalog {
using UserBatchIndex = size_t;

struct Sizes {
    // Contains the verified size for:
    // - The meta, control, and field names for previously unaccounted fields in a Bucket.
    // - Data fields after intermediate().
    int32_t uncommittedVerifiedSize = 0;

    // The estimated size of uncommitted data fields being inserted into a Bucket.
    int32_t uncommittedMeasurementEstimate = 0;

    int32_t total() const {
        return uncommittedVerifiedSize + uncommittedMeasurementEstimate;
    }

    void operator+=(const Sizes& other) {
        uncommittedVerifiedSize += other.uncommittedVerifiedSize;
        uncommittedMeasurementEstimate += other.uncommittedMeasurementEstimate;
    }
};

/**
 * The basic unit of work for a bucket. Each insert will return a shared_ptr to a WriteBatch.
 * When a writer is finished with all their insertions, they should then take steps to ensure
 * each batch they wrote into is committed. A writer commits (or aborts) the batch via
 * BucketCatalog::prepareCommit and BucketCatalog::finish.
 *
 * The order of members of this struct have been optimized for memory alignment, and therefore
 * a low memory footprint. Take extra care if modifying the order or adding or removing fields.
 */
struct WriteBatch {
    WriteBatch() = delete;
    WriteBatch(TrackingContexts& trackingContexts,
               const BucketId& bucketId,
               BucketKey bucketKey,
               OperationId opId,
               ExecutionStatsController& stats,
               std::string_view timeField);

    // Only used in testing.
    BSONObj toBSON() const;

    // True if the bucket already exists and was reopened.
    bool isReopened = false;

    // Whether the measurements in the bucket are sorted by timestamp or not.
    // True by default, if a v2 buckets gets promoted to v3 this is set to false.
    // It should not be used for v1 buckets. v2 buckets are preferred over v3 for improved
    // read/query performance, v3 buckets get created as necessary to retain higher write
    // performance.
    bool bucketIsSortedByTime = true;

    bool openedDueToMetadata =
        false;  // If true, bucket has been opened due to the inserted measurement having different
    // metadata than available buckets.

    const OperationId opId;

    uint32_t numPreviouslyCommittedMeasurements = 0;

    TrackingContexts& trackingContexts;

    std::string_view timeField;  // Necessary so we can compress on writes, since the compression
                                 // algorithm sorts on the timeField. See compressBucket().

    BSONObj min;  // Batch-local min; full if first batch, updates otherwise.
    BSONObj max;  // Batch-local max; full if first batch, updates otherwise.

    SharedPromise<void> promise;

    ExecutionStatsController stats;

    // Indices for measurements in the original user batch. Used for retryability and
    // error-handling. These two should be the same length when entering commit.
    std::vector<UserBatchIndex> userBatchIndices;
    std::vector<StmtId> stmtIds;

    // Marginal numbers for this batch only.
    // Sizes.uncommittedMeasurementEstimate is a rough estimate of data in this batch,
    // using 0 for anything under threshold, and uncompressed size for anything over threshold.
    // Sizes.uncommittedVerifiedSize is 0 until it is populated by intermediate as the delta
    // for committing this batch.
    Sizes sizes;

    StringMap<std::size_t> newFieldNamesToBeInserted;  // Value is hash of string key

    // Number of measurements we can hold in a batch without needing to allocate memory.
    static constexpr std::size_t kNumStaticBatchMeasurements = 10;
    using BatchMeasurements = boost::container::small_vector<BSONObj, kNumStaticBatchMeasurements>;
    BatchMeasurements measurements;

    const BucketId bucketId;

    /**
     * In-memory data fields. Allows for quick compression of bucket data.
     *
     * Initially these are for committed data fields, but this will be the working set of builders
     * for the current WriteBatch and will contain uncommitted data fields in
     * makeTimeseriesCompressedDiffUpdateOp.
     */
    MeasurementMap measurementMap;

    const BucketKey bucketKey;
};

const BSONObj& getUncompressedBucketDoc(const WriteBatch& batch);
void setUncompressedBucketDoc(WriteBatch& batch, BSONObj uncompressedBucketDoc);

/**
 * Returns whether the batch has already been committed or aborted.
 */
bool isWriteBatchFinished(const WriteBatch& batch);

/**
 * Retrieves the status of the write batch commit. Blocking.
 */
Status getWriteBatchStatus(WriteBatch& batch);

}  // namespace mongo::timeseries::bucket_catalog
