/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <list>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_range_deleter.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

class ScopedCollectionMetadata;

class MetadataManager {
    MONGO_DISALLOW_COPYING(MetadataManager);

public:
    using CleanupNotification = CollectionRangeDeleter::DeleteNotification;
    using Deletion = CollectionRangeDeleter::Deletion;

    MetadataManager(ServiceContext* serviceContext,
                    NamespaceString nss,
                    executor::TaskExecutor* executor);
    ~MetadataManager();

    /**
     * An ActiveMetadata must be set before this function can be called.
     *
     * Increments the usage counter of the active metadata and returns an RAII object, which
     * contains the currently active metadata.  When the usageCounter goes to zero, the RAII
     * object going out of scope will call _removeMetadata.
     */
    ScopedCollectionMetadata getActiveMetadata(std::shared_ptr<MetadataManager> self);

    /**
     * Creates the metadata on demand for a specific point in time. The object is not tracked by
     * the metadata manager.
     */
    ScopedCollectionMetadata createMetadataAt(OperationContext* opCtx, LogicalTime atCusterTime);

    /**
     * Returns the number of CollectionMetadata objects being maintained on behalf of running
     * queries.  The actual number may vary after it returns, so this is really only useful for unit
     * tests.
     */
    size_t numberOfMetadataSnapshots() const;

    /**
     * Uses the contents of the specified metadata as a way to purge any pending chunks.
     */
    void refreshActiveMetadata(std::unique_ptr<CollectionMetadata> newMetadata);

    void toBSONPending(BSONArrayBuilder& bb) const;

    /**
     * Appends information on all the chunk ranges in rangesToClean to builder.
     */
    void append(BSONObjBuilder* builder) const;

    /**
     * Schedules any documents in `range` for immediate cleanup iff no running queries can depend
     * on them, and adds the range to the list of pending ranges. Otherwise, returns a notification
     * that yields bad status immediately.  Does not block.  Call waitStatus(opCtx) on the result
     * to wait for the deletion to complete or fail.
     */
    CleanupNotification beginReceive(ChunkRange const& range);

    /**
     * Removes `range` from the list of pending ranges, and schedules any documents in the range for
     * immediate cleanup.  Does not block.  If no such range is scheduled, does nothing.
     */
    void forgetReceive(const ChunkRange& range);

    /**
     * Schedules documents in `range` for cleanup after any running queries that may depend on them
     * have terminated. Does not block. Fails if the range overlaps any current local shard chunk.
     * If `whenToDelete` is Date_t{}, deletion is scheduled immediately after the last dependent
     * query completes; otherwise, deletion is postponed until the time specified.
     *
     * Call waitStatus(opCtx) on the result to wait for the deletion to complete or fail.
     */
    CleanupNotification cleanUpRange(ChunkRange const& range, Date_t whenToDelete);

    /**
     * Returns a vector of ScopedCollectionMetadata objects representing metadata instances in use
     * by running queries that overlap the argument range, suitable for identifying and invalidating
     * those queries.
     */
    std::vector<ScopedCollectionMetadata> overlappingMetadata(
        std::shared_ptr<MetadataManager> const& itself, ChunkRange const& range);

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
     * Reports whether any range still scheduled for deletion overlaps the argument range. If so,
     * returns a notification n such that n.waitStatus(opCtx) will wake up when the newest
     * overlapping range's deletion (possibly the one of interest) completes or fails.
     */
    boost::optional<CleanupNotification> trackOrphanedDataCleanup(ChunkRange const& orphans) const;

    boost::optional<ChunkRange> getNextOrphanRange(BSONObj const& from) const;

private:
    // Management of the _metadata list is implemented in ScopedCollectionMetadata
    friend class ScopedCollectionMetadata;

    // For access to _rangesToClean and _managerLock under task callback
    friend boost::optional<Date_t> CollectionRangeDeleter::cleanUpNextRange(
        OperationContext*, NamespaceString const&, OID const&, int, CollectionRangeDeleter*);

    /**
     * Represents an instance of what the filtering metadata for this collection was at a particular
     * point in time along with a counter of how many queries are still using it.
     */
    struct CollectionMetadataTracker {
        MONGO_DISALLOW_COPYING(CollectionMetadataTracker);

        CollectionMetadataTracker(CollectionMetadata inMetadata)
            : metadata(std::move(inMetadata)) {}

        ~CollectionMetadataTracker() {
            invariant(!usageCounter);
        }

        CollectionMetadata metadata;

        std::list<Deletion> orphans;

        uint32_t usageCounter{0};
    };

    /**
     * Cancels all scheduled deletions of orphan ranges, notifying listeners with specified status.
     */
    void _clearAllCleanups(WithLock, Status);

    /**
     * Cancels all scheduled deletions of orphan ranges, notifying listeners with status
     * InterruptedDueToStepDown.
     */
    void _clearAllCleanups(WithLock);

    /**
     * Retires any metadata that has fallen out of use, and pushes any orphan ranges found in them
     * to the list of ranges actively being cleaned up.
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
     * Returns a notification if any range (possibly) still in use, but scheduled for cleanup,
     * overlaps the argument range.
     */
    boost::optional<CleanupNotification> _overlapsInUseCleanups(WithLock,
                                                                ChunkRange const& range) const;

    /**
     * Copies the argument range to the list of ranges scheduled for immediate deletion, and
     * schedules a a background task to perform the work.
     */
    CleanupNotification _pushRangeToClean(WithLock, ChunkRange const& range, Date_t when);

    /**
     * Splices the argument list elements to the list of ranges scheduled for immediate deletion,
     * and schedules a a background task to perform the work.
     */
    void _pushListToClean(WithLock, std::list<Deletion> range);

    // ServiceContext from which to obtain instances of global support objects
    ServiceContext* const _serviceContext;

    // Namespace for which this manager object applies
    const NamespaceString _nss;

    // The background task that deletes documents from orphaned chunk ranges.
    executor::TaskExecutor* const _executor;

    // Mutex to protect the state below
    mutable stdx::mutex _managerLock;

    // Contains a list of collection metadata ordered in chronological order based on the refreshes
    // that occurred. The entry at _metadata.back() is the most recent metadata and is what is
    // returned to new queries. The rest are previously active collection metadata instances still
    // in use by active server operations or cursors.
    std::list<std::shared_ptr<CollectionMetadataTracker>> _metadata;

    // Chunk ranges being migrated into to the shard. Indexed by the min key of the range.
    RangeMap _receivingChunks;

    // Ranges being deleted, or scheduled to be deleted, by a background task
    CollectionRangeDeleter _rangesToClean;
};

class ScopedCollectionMetadata {
    MONGO_DISALLOW_COPYING(ScopedCollectionMetadata);

public:
    ~ScopedCollectionMetadata() {
        _clear();
    }

    /**
     * Binds *this to the same CollectionMetadata as other, if any.
     */
    ScopedCollectionMetadata(ScopedCollectionMetadata&& other);
    ScopedCollectionMetadata& operator=(ScopedCollectionMetadata&& other);

    /**
     * Dereferencing the ScopedCollectionMetadata dereferences the private CollectionMetadata.
     */
    CollectionMetadata* getMetadata() const;

    CollectionMetadata* operator->() const {
        return getMetadata();
    }

    /**
     * True if the ScopedCollectionMetadata stores a metadata (is not empty) and the collection is
     * sharded.
     */
    operator bool() const {
        return getMetadata() != nullptr;
    }

    /**
     * Returns just the shard key fields, if collection is sharded, and the _id field, from `doc`.
     * Does not alter any field values (e.g. by hashing); values are copied verbatim.
     */
    BSONObj extractDocumentKey(BSONObj const& doc) const;

private:
    friend ScopedCollectionMetadata MetadataManager::getActiveMetadata(
        std::shared_ptr<MetadataManager>);

    friend ScopedCollectionMetadata MetadataManager::createMetadataAt(OperationContext*,
                                                                      LogicalTime);

    friend std::vector<ScopedCollectionMetadata> MetadataManager::overlappingMetadata(
        std::shared_ptr<MetadataManager> const&, ChunkRange const&);

    /**
     * Creates an empty ScopedCollectionMetadata, which is interpreted as if the collection is
     * unsharded.
     */
    ScopedCollectionMetadata();

    /**
     * Increments the usageCounter in the specified CollectionMetadata.
     *
     * Must be called with manager->_managerLock held.  Arguments must be non-null.
     */
    ScopedCollectionMetadata(
        WithLock,
        std::shared_ptr<MetadataManager> metadataManager,
        std::shared_ptr<MetadataManager::CollectionMetadataTracker> metadataTracker);

    /**
     * Metadata not tracked by the manager - created on demand for a specific point in time.
     */
    ScopedCollectionMetadata(
        std::shared_ptr<MetadataManager::CollectionMetadataTracker> metadataTracker);

    /**
     * Disconnect from the CollectionMetadata, possibly triggering GC of unused CollectionMetadata.
     */
    void _clear();

    std::shared_ptr<MetadataManager> _metadataManager;

    std::shared_ptr<MetadataManager::CollectionMetadataTracker> _metadataTracker;
};

}  // namespace mongo
