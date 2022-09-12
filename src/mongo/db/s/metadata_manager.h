/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <list>
#include <memory>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

class RangePreserver;

/**
 * Contains filtering metadata for a sharded collection.
 */
class MetadataManager : public std::enable_shared_from_this<MetadataManager> {
public:
    MetadataManager(ServiceContext* serviceContext,
                    NamespaceString nss,
                    std::shared_ptr<executor::TaskExecutor> executor,
                    CollectionMetadata initialMetadata);
    ~MetadataManager() = default;

    MetadataManager(const MetadataManager&) = delete;
    MetadataManager& operator=(const MetadataManager&) = delete;

    /**
     * Increments the usage counter of the active metadata and returns an RAII object, which
     * corresponds to it.
     *
     * Holding a reference on a particular instance of the metadata means that orphan cleanup is not
     * allowed to run and delete chunks which are covered by that metadata. When the returned
     * ScopedCollectionDescription goes out of scope, the reference counter on the metadata will be
     * decremented and if it reaches to zero, orphan cleanup may proceed.
     */
    std::shared_ptr<ScopedCollectionDescription::Impl> getActiveMetadata(
        const boost::optional<LogicalTime>& atClusterTime);

    /**
     * Returns the shard version of the active metadata object.
     */
    ChunkVersion getActiveShardVersion() {
        stdx::lock_guard<Latch> lg(_managerLock);
        invariant(!_metadata.empty());
        return _metadata.back()->metadata->getShardVersion();
    }

    /**
     * Returns the UUID of the collection tracked by this MetadataManager object.
     */
    UUID getCollectionUuid() const {
        return _collectionUuid;
    }

    /**
     * Returns the number of CollectionMetadata objects being maintained on behalf of running
     * queries.  The actual number may vary after it returns, so this is really only useful for unit
     * tests.
     */
    size_t numberOfMetadataSnapshots() const;

    /**
     * Returns the number of metadata objects that have been set to boost::none in
     * _retireExpiredMetadata(). The actual number may vary after it returns, so this is really only
     * useful for unit tests.
     */
    int numberOfEmptyMetadataSnapshots() const;

    void setFilteringMetadata(CollectionMetadata newMetadata);

    /**
     * Appends information on all the chunk ranges in rangesToClean to builder.
     */
    void append(BSONObjBuilder* builder) const;

    /**
     * Schedules documents in `range` for cleanup after any running queries that may depend on them
     * have terminated. Does not block. Fails if the range overlaps any current local shard chunk.
     *
     * If shouldDelayBeforeDeletion is false, deletion is scheduled immediately after the last
     * dependent query completes; otherwise, deletion is postponed until after
     * orphanCleanupDelaySecs after the last dependent query completes.
     *
     * Returns a future that will be fulfilled when the range deletion completes or fails.
     */
    SharedSemiFuture<void> cleanUpRange(ChunkRange const& range, bool shouldDelayBeforeDeletion);

    /**
     * Returns the number of ranges scheduled to be cleaned, exclusive of such ranges that might
     * still be in use by running queries.  Outside of test drivers, the actual number may vary
     * after it returns, so this is really only useful for unit tests.
     */
    size_t numberOfRangesToClean() const;

    /**
     * Returns the number of ranges scheduled to be cleaned once all queries that could depend on
     * them have terminated. The actual number may vary after it returns, so this is really only
     * useful for unit tests.
     */
    size_t numberOfRangesToCleanStillInUse() const;

    /**
     * Returns the number of ranges scheduled for deletion, regardless of whether they may still be
     * in use by running queries.
     */
    size_t numberOfRangesScheduledForDeletion() const;

    /**
     * Reports whether any range still scheduled for deletion overlaps the argument range. If so,
     * returns a future that will be resolved when the newest overlapping range's deletion (possibly
     * the one of interest) completes or fails.
     */
    boost::optional<SharedSemiFuture<void>> trackOrphanedDataCleanup(
        ChunkRange const& orphans) const;

private:
    // Management of the _metadata list is implemented in RangePreserver
    friend class RangePreserver;

    /**
     * Represents an instance of what the filtering metadata for this collection was at a particular
     * point in time along with a counter of how many queries are still using it.
     */
    struct CollectionMetadataTracker {
        CollectionMetadataTracker(const CollectionMetadataTracker&) = delete;
        CollectionMetadataTracker& operator=(const CollectionMetadataTracker&) = delete;

        CollectionMetadataTracker(CollectionMetadata inMetadata)
            : metadata(std::move(inMetadata)) {}

        ~CollectionMetadataTracker() {
            invariant(!usageCounter);
            onDestructionPromise.emplaceValue();
        }

        boost::optional<CollectionMetadata> metadata;

        /**
         * Number of range deletion tasks waiting on this CollectionMetadataTracker to be destroyed
         * before deleting documents.
         */
        uint32_t numContingentRangeDeletionTasks{0};

        /**
         * Promise that will be signaled when this object is destroyed.
         *
         * In the case where this CollectionMetadataTracker may refer to orphaned documents for one
         * or more ranges, the corresponding futures from this promise are used as barriers to
         * prevent range deletion tasks for those ranges from proceeding until this object is
         * destroyed, to guarantee that ranges aren't deleted while active queries can still access
         * them.
         */
        SharedPromise<void> onDestructionPromise;

        uint32_t usageCounter{0};
    };

    /**
     * Retires any metadata that has fallen out of use, potentially allowing range deletions to
     * proceed which were waiting for active queries using these metadata objects to complete.
     */
    void _retireExpiredMetadata(WithLock);

    /**
     * Pushes current set of chunks, if any, to _metadataInUse, replaces it with newMetadata.
     */
    void _setActiveMetadata(WithLock wl, CollectionMetadata newMetadata);

    /**
     * Finds the most-recently pushed metadata that might depend on `range`, or nullptr if none.
     * The result is usable until the lock is released.
     */
    CollectionMetadataTracker* _findNewestOverlappingMetadata(WithLock, ChunkRange const& range);

    /**
     * Returns true if the specified range overlaps any chunk that might be currently in use by a
     * running query.
     */

    bool _overlapsInUseChunk(WithLock, ChunkRange const& range);

    /**
     * Schedule a task to delete the given range of documents once waitForActiveQueriesToComplete
     * has been signaled, and store the resulting future for the task in
     * _rangesScheduledForDeletion.
     */
    SharedSemiFuture<void> _submitRangeForDeletion(
        const WithLock&,
        SemiFuture<void> waitForActiveQueriesToComplete,
        const ChunkRange& range,
        Seconds delayForActiveQueriesOnSecondariesToComplete);

    // ServiceContext from which to obtain instances of global support objects
    ServiceContext* const _serviceContext;

    // Namespace for which this manager object applies
    const NamespaceString _nss;

    // The UUID for the collection tracked by this manager object.
    const UUID _collectionUuid;

    // The background task that deletes documents from orphaned chunk ranges.
    std::shared_ptr<executor::TaskExecutor> const _executor;

    // Mutex to protect the state below
    mutable Mutex _managerLock = MONGO_MAKE_LATCH("MetadataManager::_managerLock");

    // Contains a list of collection metadata for the same collection uuid, ordered in
    // chronological order based on the refreshes that occurred. The entry at _metadata.back() is
    // the most recent metadata and is what is returned to new queries. The rest are previously
    // active collection metadata instances still in use by active server operations or cursors.
    std::list<std::shared_ptr<CollectionMetadataTracker>> _metadata;

    // Ranges being deleted, or scheduled to be deleted, by a background task.
    std::list<std::pair<ChunkRange, SharedSemiFuture<void>>> _rangesScheduledForDeletion;
};

}  // namespace mongo
