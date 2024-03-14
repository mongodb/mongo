/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <absl/container/inlined_vector.h>
#include <boost/container/static_vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <variant>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/bucket_catalog/bucket.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"
#include "mongo/db/timeseries/bucket_catalog/closed_bucket.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/bucket_catalog/reopening.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/views/view.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo::timeseries::bucket_catalog {

using StripeNumber = std::uint8_t;
using ShouldClearFn = std::function<bool(const UUID&)>;

/**
 * Whether to allow inserts to be batched together with those from other clients.
 */
enum class CombineWithInsertsFromOtherClients {
    kAllow,
    kDisallow,
};

/**
 * Return type indicating that a call to 'insert' or 'tryInsert' successfully staged the input
 * measurement for insertion. See 'insert' and 'tryInsert' for more information.
 */
class SuccessfulInsertion {
public:
    SuccessfulInsertion() = default;
    SuccessfulInsertion(SuccessfulInsertion&&) = default;
    SuccessfulInsertion& operator=(SuccessfulInsertion&&) = default;
    SuccessfulInsertion(const SuccessfulInsertion&) = delete;
    SuccessfulInsertion& operator=(const SuccessfulInsertion&) = delete;
    SuccessfulInsertion(std::shared_ptr<WriteBatch>&&, ClosedBuckets&&);

    std::shared_ptr<WriteBatch> batch;
    ClosedBuckets closedBuckets;
};

/**
 * Return type indicating that a call to 'tryInsert' must retry after waiting for a conflicting
 * operation to resolve. Caller should wait using 'waitToInsert'.
 *
 * In particular, if 'tryInsert' would have generated a 'ReopeningContext', but there is already an
 * outstanding 'ReopeningRequest' or a prepared 'WriteBatch' for a bucket in the series (same
 * metaField value), that represents a conflict.
 */
using InsertWaiter = std::variant<std::shared_ptr<WriteBatch>, std::shared_ptr<ReopeningRequest>>;

/**
 * Variant representing the possible outcomes of 'tryInsert' or 'insert'. See 'tryInsert' and
 * 'insert' for more details.
 */
using InsertResult = std::variant<SuccessfulInsertion, ReopeningContext, InsertWaiter>;

/**
 * Struct to hold a portion of the buckets managed by the catalog.
 *
 * Each of the bucket lists, as well as the buckets themselves, are protected by 'mutex'.
 */
struct Stripe {
    // All access to a stripe should happen while 'mutex' is locked.
    mutable Mutex mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(1), "BucketCatalog::Stripe::mutex");

    // All buckets currently open in the catalog, including buckets which are full or pending
    // closure but not yet committed, indexed by BucketId. Owning pointers.
    tracked_unordered_map<BucketId, unique_tracked_ptr<Bucket>, BucketHasher> openBucketsById;

    // All buckets currently open in the catalog, including buckets which are full or pending
    // closure but not yet committed, indexed by BucketKey. Non-owning pointers.
    tracked_unordered_map<BucketKey, tracked_set<Bucket*>, BucketHasher> openBucketsByKey;

    // Open buckets that do not have any outstanding writes.
    using IdleList = tracked_list<Bucket*>;
    IdleList idleBuckets;

    // Buckets that are not currently in the catalog, but which are eligible to receive more
    // measurements. The top-level map is keyed by the hash of the BucketKey, while the stored
    // map is keyed by the bucket's minimum timestamp.
    //
    // We invert the key comparison in the inner map so that we can use lower_bound to efficiently
    // find an archived bucket that is a candidate for an incoming measurement.
    tracked_unordered_map<BucketKey::Hash,
                          tracked_map<Date_t, ArchivedBucket, std::greater<Date_t>>,
                          BucketHasher>
        archivedBuckets;

    // All series currently with outstanding reopening operations. Used to coordinate disk access
    // between reopenings and regular writes to prevent stale reads and corrupted updates.
    static constexpr int kInlinedVectorSize = 4;
    tracked_unordered_map<
        BucketKey,
        tracked_inlined_vector<shared_tracked_ptr<ReopeningRequest>, kInlinedVectorSize>,
        BucketHasher>
        outstandingReopeningRequests;

    Stripe(TrackingContext& trackingContext);
};

/**
 * This class holds all the data used to coordinate and combine time series inserts amongst threads.
 */
class BucketCatalog {
public:
    static BucketCatalog& get(ServiceContext* svcCtx);
    static BucketCatalog& get(OperationContext* opCtx);

    BucketCatalog();
    BucketCatalog(size_t numberOfStripes, std::function<uint64_t()> memoryUsageThreshold);
    BucketCatalog(const BucketCatalog&) = delete;
    BucketCatalog operator=(const BucketCatalog&) = delete;

    // Stores an accurate count of the bytes allocated and deallocated for all the data held by
    // tracked members of the BucketCatalog.
    TrackingContext trackingContext;

    // Stores state information about all buckets managed by the catalog, across stripes.
    BucketStateRegistry bucketStateRegistry;

    // The actual buckets in the catalog are distributed across a number of 'Stripe's. Each can be
    // independently locked and operated on in parallel. The size of the stripe vector should not be
    // changed after initialization.
    const std::size_t numberOfStripes = 32;
    tracked_vector<unique_tracked_ptr<Stripe>> stripes;

    // Per-namespace execution stats. This map is protected by 'mutex'. Once you complete your
    // lookup, you can keep the shared_ptr to an individual namespace's stats object and release the
    // lock. The object itself is thread-safe (using atomics).
    mutable Mutex mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "BucketCatalog::mutex");
    tracked_unordered_map<UUID, shared_tracked_ptr<ExecutionStats>> executionStats;

    // Global execution stats used to report aggregated metrics in server status.
    ExecutionStats globalExecutionStats;

    // Memory usage threshold in bytes after which idle buckets will be expired.
    std::function<uint64_t()> memoryUsageThreshold;

    // Cardinality of opened and archived buckets managed across all stripes.
    AtomicWord<uint32_t> numberOfActiveBuckets;
};

/**
 * Returns the metadata for the given bucket in the following format:
 *     {<metadata field name>: <value>}
 * All measurements in the given bucket share same metadata value.
 *
 * Returns an empty document if the given bucket cannot be found or if this time-series collection
 * was not created with a metadata field name.
 */
BSONObj getMetadata(BucketCatalog& catalog, const BucketHandle& bucket);

/**
 * Returns the memory usage of the bucket catalog across all stripes from the approximated memory
 * usage, and the tracked memory usage from the TrackingAllocator.
 */
uint64_t getMemoryUsage(const BucketCatalog& catalog);

/**
 * Tries to insert 'doc' into a suitable bucket. If an open bucket is full (or has incompatible
 * schema), but is otherwise suitable, we will close it and open a new bucket. If we find no bucket
 * with matching data and a time range that can accommodate 'doc', we will not open a new bucket,
 * but rather let the caller know to either
 *  - search for an archived or closed bucket that can accommodate 'doc' by returning a
 *    'ReopeningContext', or
 *  - retry the insert after waiting on the returned 'InsertWaiter'.
 *
 * If a suitable bucket is found or opened, returns a 'SuccessfulInsertion' containing the
 * 'WriteBatch' into which 'doc' was inserted and a list of any buckets that were closed to make
 * space to insert 'doc'. Any caller who receives the same batch may commit or abort the batch after
 * claiming commit rights. See 'WriteBatch' for more details.
 *
 * If a 'ReopeningContext' is returned, it contains either a bucket ID, corresponding to an archived
 * bucket which should be fetched, an aggregation pipeline that can be used to search for a
 * previously-closed bucket that can accommodate 'doc', or (in hopefully rare cases) a
 * std::monostate which requires no intermediate action, The caller should then proceed to call
 * 'insert' to insert 'doc', passing any fetched bucket back as a member of the 'ReopeningContext'.
 */
StatusWith<InsertResult> tryInsert(OperationContext* opCtx,
                                   BucketCatalog& catalog,
                                   const NamespaceString& nss,
                                   const UUID& collectionUUID,
                                   const StringDataComparator* comparator,
                                   const TimeseriesOptions& options,
                                   const BSONObj& doc,
                                   CombineWithInsertsFromOtherClients combine);

/**
 * Returns the WriteBatch into which the document was inserted and a list of any buckets that were
 * closed in order to make space to insert the document. Any caller who receives the same batch may
 * commit or abort the batch after claiming commit rights. See WriteBatch for more details.
 *
 * We will attempt to reopen the bucket passed via 'reopeningContext' and attempt to add
 * 'doc' to that bucket. Otherwise we will attempt to find a suitable open bucket, or open a new
 * bucket if none exists.
 */
StatusWith<InsertResult> insertWithReopeningContext(OperationContext* opCtx,
                                                    BucketCatalog& catalog,
                                                    const NamespaceString& nss,
                                                    const UUID& collectionUUID,
                                                    const StringDataComparator* comparator,
                                                    const TimeseriesOptions& options,
                                                    const BSONObj& doc,
                                                    CombineWithInsertsFromOtherClients combine,
                                                    ReopeningContext& reopeningContext);

/**
 * Returns the WriteBatch into which the document was inserted and a list of any buckets that were
 * closed in order to make space to insert the document. Any caller who receives the same batch may
 * commit or abort the batch after claiming commit rights. See WriteBatch for more details.
 *
 * We will attempt to find a suitable open bucket, or open a new bucket if none exists.
 */
StatusWith<InsertResult> insert(OperationContext* opCtx,
                                BucketCatalog& catalog,
                                const NamespaceString& nss,
                                const UUID& collectionUUID,
                                const StringDataComparator* comparator,
                                const TimeseriesOptions& options,
                                const BSONObj& doc,
                                CombineWithInsertsFromOtherClients combine);

/**
 * If a 'tryInsert' call returns a 'InsertWaiter' object, the caller should use this function to
 * wait before repeating their attempt.
 */
void waitToInsert(InsertWaiter* waiter);

/**
 * Prepares a batch for commit, transitioning it to an inactive state. Caller must already have
 * commit rights on batch. Returns OK if the batch was successfully prepared, or a status indicating
 * why the batch was previously aborted by another operation. If another batch is already prepared
 * on the same bucket, or there is an outstanding 'ReopeningRequest' for the same series (metaField
 * value), this operation will block waiting for it to complete.
 */
Status prepareCommit(BucketCatalog& catalog,
                     const NamespaceString& nss,
                     std::shared_ptr<WriteBatch> batch);

/**
 * Records the result of a batch commit. Caller must already have commit rights on batch, and batch
 * must have been previously prepared.
 *
 * Returns bucket information of a bucket if one was closed.
 *
 * Debug builds will attempt to verify the resulting bucket contents on disk if passed an 'opCtx'.
 */
boost::optional<ClosedBucket> finish(OperationContext* opCtx,
                                     BucketCatalog& catalog,
                                     const NamespaceString& nss,
                                     std::shared_ptr<WriteBatch> batch,
                                     const CommitInfo& info);

/**
 * Aborts the given write batch and any other outstanding (unprepared) batches on the same bucket,
 * using the provided status.
 */
void abort(BucketCatalog& catalog, std::shared_ptr<WriteBatch> batch, const Status& status);

/**
 * Notifies the catalog of a direct write (that is, a write not initiated by the BucketCatalog) that
 * will be performed on the bucket document with the specified ID. If there is already an
 * internally-prepared operation on that bucket, this method will throw a 'WriteConflictException'.
 * This should be followed by a call to 'directWriteFinish' after the write has been committed,
 * rolled back, or otherwise finished.
 */
void directWriteStart(BucketStateRegistry& registry, const UUID& collectionUUID, const OID& oid);

/**
 * Notifies the catalog that a pending direct write to the bucket document with the specified ID has
 * finished or been abandoned, and normal operations on the bucket can resume. After this point any
 * in-memory representation of the on-disk bucket data from before the direct write should have been
 * cleared from the catalog, and it may be safely reopened from the on-disk state.
 */
void directWriteFinish(BucketStateRegistry& registry, const UUID& collectionUUID, const OID& oid);

/**
 * Clears any bucket whose collection UUID has been cleared by removing the bucket from the catalog
 * asynchronously through the BucketStateRegistry.
 */
void clear(BucketCatalog& catalog, tracked_vector<UUID> clearedCollectionUUIDs);

/**
 * Clears the buckets for the given collection UUID by removing the bucket from the catalog
 * asynchronously through the BucketStateRegistry.
 */
void clear(BucketCatalog& catalog, const UUID& collectionUUID);

/**
 * Freezes the given bucket in the registry so that this bucket will never be used in the future.
 */
void freeze(BucketCatalog&, const UUID&, const OID&);

/**
 * Resets the counter used for bucket OID generation. Should be called after a bucket _id collision.
 */
void resetBucketOIDCounter();

/**
 * Appends the execution stats for the given namespace to the builder.
 */
void appendExecutionStats(const BucketCatalog& catalog,
                          const UUID& collectionUUID,
                          BSONObjBuilder& builder);

/**
 * Reports a number of measurements inserted that were committed by a different thread than the one
 * that initially staged them. These measurements are considered to have benefitted from "group
 * commit".
 */
void reportMeasurementsGroupCommitted(BucketCatalog& catalog,
                                      const UUID& collectionUUID,
                                      int64_t count);


}  // namespace mongo::timeseries::bucket_catalog
