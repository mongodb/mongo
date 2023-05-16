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

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <queue>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/bucket_catalog/bucket.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"
#include "mongo/db/timeseries/bucket_catalog/closed_bucket.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/bucket_catalog/reopening.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/string_map.h"

namespace mongo::timeseries::bucket_catalog {

using StripeNumber = std::uint8_t;
using ShouldClearFn = std::function<bool(const NamespaceString&)>;

/**
 * Whether to allow inserts to be batched together with those from other clients.
 */
enum class CombineWithInsertsFromOtherClients {
    kAllow,
    kDisallow,
};

/**
 * Return type for the insert functions. See insert() and tryInsert() for more information.
 */
class InsertResult {
public:
    InsertResult() = default;
    InsertResult(InsertResult&&) = default;
    InsertResult& operator=(InsertResult&&) = default;
    InsertResult(const InsertResult&) = delete;
    InsertResult& operator=(const InsertResult&) = delete;

    std::shared_ptr<WriteBatch> batch;
    ClosedBuckets closedBuckets;
    stdx::variant<std::monostate, OID, std::vector<BSONObj>> candidate;
    uint64_t catalogEra = 0;
};

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
    stdx::unordered_map<BucketId, std::unique_ptr<Bucket>, BucketHasher> openBucketsById;

    // All buckets currently open in the catalog, including buckets which are full or pending
    // closure but not yet committed, indexed by BucketKey. Non-owning pointers.
    stdx::unordered_map<BucketKey, std::set<Bucket*>, BucketHasher> openBucketsByKey;

    // Open buckets that do not have any outstanding writes.
    using IdleList = std::list<Bucket*>;
    IdleList idleBuckets;

    // Buckets that are not currently in the catalog, but which are eligible to receive more
    // measurements. The top-level map is keyed by the hash of the BucketKey, while the stored
    // map is keyed by the bucket's minimum timestamp.
    //
    // We invert the key comparison in the inner map so that we can use lower_bound to efficiently
    // find an archived bucket that is a candidate for an incoming measurement.
    stdx::unordered_map<BucketKey::Hash,
                        std::map<Date_t, ArchivedBucket, std::greater<Date_t>>,
                        BucketHasher>
        archivedBuckets;
};

/**
 * This class holds all the data used to coordinate and combine time series inserts amongst threads.
 */
class BucketCatalog {
public:
    static BucketCatalog& get(ServiceContext* svcCtx);
    static BucketCatalog& get(OperationContext* opCtx);

    BucketCatalog() : stripes(numberOfStripes) {}
    BucketCatalog(size_t numberOfStripes)
        : numberOfStripes(numberOfStripes), stripes(numberOfStripes){};
    BucketCatalog(const BucketCatalog&) = delete;
    BucketCatalog operator=(const BucketCatalog&) = delete;

    // Stores state information about all buckets managed by the catalog, across stripes.
    BucketStateRegistry bucketStateRegistry;

    // The actual buckets in the catalog are distributed across a number of 'Stripe's. Each can be
    // independently locked and operated on in parallel. The size of the stripe vector should not be
    // changed after initialization.
    const std::size_t numberOfStripes = 32;
    std::vector<Stripe> stripes;

    // Per-namespace execution stats. This map is protected by 'mutex'. Once you complete your
    // lookup, you can keep the shared_ptr to an individual namespace's stats object and release the
    // lock. The object itself is thread-safe (using atomics).
    mutable Mutex mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "BucketCatalog::mutex");
    stdx::unordered_map<NamespaceString, std::shared_ptr<ExecutionStats>> executionStats;

    // Global execution stats used to report aggregated metrics in server status.
    ExecutionStats globalExecutionStats;

    // Approximate memory usage of the bucket catalog across all stripes.
    AtomicWord<uint64_t> memoryUsage;

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
 * Tries to insert 'doc' into a suitable bucket. If an open bucket is full (or has incompatible
 * schema), but is otherwise suitable, we will close it and open a new bucket. If we find no bucket
 * with matching data and a time range that can accommodate 'doc', we will not open a new bucket,
 * but rather let the caller know to search for an archived or closed bucket that can accommodate
 * 'doc'.
 *
 * If a suitable bucket is found or opened, returns the WriteBatch into which 'doc' was inserted and
 * a list of any buckets that were closed to make space to insert 'doc'. Any caller who receives the
 * same batch may commit or abort the batch after claiming commit rights. See WriteBatch for more
 * details.
 *
 * If no suitable bucket is found or opened, returns an optional bucket ID. If set, the bucket ID
 * corresponds to an archived bucket which should be fetched; otherwise the caller should search for
 * a previously-closed bucket that can accommodate 'doc'. The caller should proceed to call 'insert'
 * to insert 'doc', passing any fetched bucket.
 */
StatusWith<InsertResult> tryInsert(OperationContext* opCtx,
                                   BucketCatalog& catalog,
                                   const NamespaceString& ns,
                                   const StringData::ComparatorInterface* comparator,
                                   const TimeseriesOptions& options,
                                   const BSONObj& doc,
                                   CombineWithInsertsFromOtherClients combine);

/**
 * Returns the WriteBatch into which the document was inserted and a list of any buckets that were
 * closed in order to make space to insert the document. Any caller who receives the same batch may
 * commit or abort the batch after claiming commit rights. See WriteBatch for more details.
 *
 * If 'bucketToReopen' is passed, we will reopen that bucket and attempt to add 'doc' to that
 * bucket. Otherwise we will attempt to find a suitable open bucket, or open a new bucket if none
 * exists.
 */
StatusWith<InsertResult> insert(OperationContext* opCtx,
                                BucketCatalog& catalog,
                                const NamespaceString& ns,
                                const StringData::ComparatorInterface* comparator,
                                const TimeseriesOptions& options,
                                const BSONObj& doc,
                                CombineWithInsertsFromOtherClients combine,
                                BucketFindResult bucketFindResult = {});

/**
 * Prepares a batch for commit, transitioning it to an inactive state. Caller must already have
 * commit rights on batch. Returns OK if the batch was successfully prepared, or a status indicating
 * why the batch was previously aborted by another operation. If another batch is already prepared,
 * this operation will block waiting for it to complete.
 */
Status prepareCommit(BucketCatalog& catalog, std::shared_ptr<WriteBatch> batch);

/**
 * Records the result of a batch commit. Caller must already have commit rights on batch, and batch
 * must have been previously prepared.
 *
 * Returns bucket information of a bucket if one was closed.
 */
boost::optional<ClosedBucket> finish(BucketCatalog& catalog,
                                     std::shared_ptr<WriteBatch> batch,
                                     const CommitInfo& info);

/**
 * Aborts the given write batch and any other outstanding batches on the same bucket, using the
 * provided status.
 */
void abort(BucketCatalog& catalog, std::shared_ptr<WriteBatch> batch, const Status& status);

/**
 * Notifies the catalog of a direct write (that is, a write not initiated by the BucketCatalog) that
 * will be performed on the bucket document with the specified ID. If there is already an
 * internally-prepared operation on that bucket, this method will throw a 'WriteConflictException'.
 * This should be followed by a call to 'directWriteFinish' after the write has been committed,
 * rolled back, or otherwise finished.
 */
void directWriteStart(BucketStateRegistry& registry, const NamespaceString& ns, const OID& oid);

/**
 * Notifies the catalog that a pending direct write to the bucket document with the specified ID has
 * finished or been abandoned, and normal operations on the bucket can resume. After this point any
 * in-memory representation of the on-disk bucket data from before the direct write should have been
 * cleared from the catalog, and it may be safely reopened from the on-disk state.
 */
void directWriteFinish(BucketStateRegistry& registry, const NamespaceString& ns, const OID& oid);

/**
 * Clears any bucket whose namespace satisfies the predicate by removing the bucket from the catalog
 * asynchronously through the BucketStateRegistry.
 */
void clear(BucketCatalog& catalog, ShouldClearFn&& shouldClear);

/**
 * Clears the buckets for the given namespace by removing the bucket from the catalog asynchronously
 * through the BucketStateRegistry.
 */
void clear(BucketCatalog& catalog, const NamespaceString& ns);

/**
 * Clears the buckets for the given database by removing the bucket from the catalog asynchronously
 * through the BucketStateRegistry.
 */
void clear(BucketCatalog& catalog, const DatabaseName& dbName);

/**
 * Appends the execution stats for the given namespace to the builder.
 */
void appendExecutionStats(const BucketCatalog& catalog,
                          const NamespaceString& ns,
                          BSONObjBuilder& builder);

}  // namespace mongo::timeseries::bucket_catalog
