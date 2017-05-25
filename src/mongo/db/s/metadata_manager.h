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

namespace mongo {

class ScopedCollectionMetadata;

class MetadataManager {
    MONGO_DISALLOW_COPYING(MetadataManager);

public:
    using CleanupNotification = CollectionRangeDeleter::DeleteNotification;

    MetadataManager(ServiceContext*, NamespaceString nss, executor::TaskExecutor* rangeDeleter);
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
     * Returns the number of CollectionMetadata objects being maintained on behalf of running
     * queries.  The actual number may vary after it returns, so this is really only useful for unit
     * tests.
     */
    size_t numberOfMetadataSnapshots();

    /**
     * Uses the contents of the specified metadata as a way to purge any pending chunks.
     */
    void refreshActiveMetadata(std::unique_ptr<CollectionMetadata> newMetadata);

    void toBSONPending(BSONArrayBuilder& bb) const;

    /**
     * Appends information on all the chunk ranges in rangesToClean to builder.
     */
    void append(BSONObjBuilder* builder);

    /**
     * Returns a map to the set of chunks being migrated in.
     */
    RangeMap const& getReceiveMap() const {
        return _receivingChunks;
    }

    /**
     * If no running queries can depend on documents in the range, schedules any such documents for
     * immediate cleanup. Otherwise, returns false.
     */
    CleanupNotification beginReceive(ChunkRange const& range);

    /**
     * Removes the range from the pending list, and schedules any documents in the range for
     * immediate cleanup.  Assumes no active queries can see any local documents in the range.
     */
    void forgetReceive(const ChunkRange& range);

    /**
     * Initiates cleanup of the orphaned documents as if a chunk has been migrated out. If any
     * documents in the range might still be in use by running queries, queues cleanup to begin
     * after they have all terminated.  Otherwise, schedules documents for immediate cleanup.
     * Fails if the range overlaps any current local shard chunk.
     *
     * Must be called with the collection locked for writing.  To monitor completion, use
     * trackOrphanedDataCleanup or CollectionShardingState::waitForClean.
     */
    CleanupNotification cleanUpRange(ChunkRange const& range);

    /**
     * Returns a vector of ScopedCollectionMetadata objects representing metadata instances in use
     * by running queries that overlap the argument range, suitable for identifying and invalidating
     * those queries.
     */
    auto overlappingMetadata(std::shared_ptr<MetadataManager> const& itself,
                             ChunkRange const& range) -> std::vector<ScopedCollectionMetadata>;

    /**
     * Returns the number of ranges scheduled to be cleaned, exclusive of such ranges that might
     * still be in use by running queries.  Outside of test drivers, the actual number may vary
     * after it returns, so this is really only useful for unit tests.
     */
    size_t numberOfRangesToClean();

    /**
     * Returns the number of ranges scheduled to be cleaned once all queries that could depend on
     * them have terminated. The actual number may vary after it returns, so this is really only
     * useful for unit tests.
     */
    size_t numberOfRangesToCleanStillInUse();

    /**
     * Reports whether any range still scheduled for deletion overlaps the argument range. If so,
     * returns a notification n such that n.waitStatus(opCtx) will wake up when the newest
     * overlapping range's deletion (possibly the one of interest) completes or fails.
     */
    boost::optional<CleanupNotification> trackOrphanedDataCleanup(ChunkRange const& orphans);

    boost::optional<KeyRange> getNextOrphanRange(BSONObj const& from);

    using Deletion = CollectionRangeDeleter::Deletion;

private:
    /**
     * Deletes ranges, in background, until done, normally using a task executor attached to the
     * ShardingState.
     *
     * Each time it completes cleaning up a range, it wakes up clients waiting on completion of
     * that range, which may then verify their range has no more deletions scheduled, and proceed.
     */
    static void _scheduleCleanup(executor::TaskExecutor*,
                                 NamespaceString nss,
                                 CollectionRangeDeleter::Action);

    // All of the following functions must be called while holding _managerLock.

    /**
     * Cancels all scheduled deletions of orphan ranges, notifying listeners with specified status.
     */
    void _clearAllCleanups(Status);

    /**
     * Cancels all scheduled deletions of orphan ranges, notifying listeners with status
     * InterruptedDueToReplStateChange.
     */
    void _clearAllCleanups();

    /**
     * Retires any metadata that has fallen out of use, and pushes any orphan ranges found in them
     * to the list of ranges actively being cleaned up.
     */
    void _retireExpiredMetadata();

    /**
     * Pushes current set of chunks, if any, to _metadataInUse, replaces it with newMetadata.
     */
    void _setActiveMetadata_inlock(std::unique_ptr<CollectionMetadata> newMetadata);

    /**
     * Returns true if the specified range overlaps any chunk that might be currently in use by a
     * running query.
     */

    bool _overlapsInUseChunk(ChunkRange const& range);

    /**
     * Returns true if any range (possibly) still in use, but scheduled for cleanup, overlaps
     * the argument range.
     */
    auto _overlapsInUseCleanups(ChunkRange const& range) -> boost::optional<CleanupNotification>;

    /**
     * Copies the argument range to the list of ranges scheduled for immediate deletion, and
     * schedules a a background task to perform the work.
     */
    CleanupNotification _pushRangeToClean(ChunkRange const& range);

    /**
     * Splices the argument list elements to the list of ranges scheduled for immediate deletion,
     * and schedules a a background task to perform the work.
     */
    void _pushListToClean(std::list<Deletion> range);

    /**
     * Adds a range from the receiving map, so getNextOrphanRange will skip ranges migrating in.
     */
    void _addToReceiving(ChunkRange const& range);

    /**
     * Removes a range from the receiving map after a migration failure. The range must
     * exactly match an element of _receivingChunks.
     */
    void _removeFromReceiving(ChunkRange const& range);

    // data members

    const NamespaceString _nss;
    // ServiceContext from which to obtain instances of global support objects.
    ServiceContext* const _serviceContext;

    // Mutex to protect the state below
    stdx::mutex _managerLock;

    // _metadata.back() is the collection metadata reflecting chunks accessible to new queries.
    // The rest are previously active collection metadata instances still in use by active server
    // operations or cursors.
    std::list<std::shared_ptr<CollectionMetadata>> _metadata;

    // Chunk ranges being migrated into to the shard. Indexed by the min key of the range.
    RangeMap _receivingChunks;

    // The background task that deletes documents from orphaned chunk ranges.
    executor::TaskExecutor* const _executor;

    // Ranges being deleted, or scheduled to be deleted, by a background task
    CollectionRangeDeleter _rangesToClean;

    // friends

    // for access to _rangesToClean and _managerLock under task callback
    friend auto CollectionRangeDeleter::cleanUpNextRange(OperationContext*,
                                                         NamespaceString const&,
                                                         CollectionRangeDeleter::Action,
                                                         int maxToDelete,
                                                         CollectionRangeDeleter*)
        -> CollectionRangeDeleter::Action;
    friend class ScopedCollectionMetadata;
};

class ScopedCollectionMetadata {
    MONGO_DISALLOW_COPYING(ScopedCollectionMetadata);

public:
    /**
     * Creates an empty ScopedCollectionMetadata. Using the default constructor means that no
     * metadata is available.
     */
    ScopedCollectionMetadata() = default;
    ~ScopedCollectionMetadata();

    /**
     * Binds *this to the same CollectionMetadata as other, if any.
     */
    ScopedCollectionMetadata(ScopedCollectionMetadata&& other);
    ScopedCollectionMetadata& operator=(ScopedCollectionMetadata&& other);

    /**
     * Dereferencing the ScopedCollectionMetadata dereferences the private CollectionMetadata.
     */
    CollectionMetadata* operator->() const;
    CollectionMetadata* getMetadata() const;

    /**
     * True if the ScopedCollectionMetadata stores a metadata (is not empty) and the collection is
     * sharded.
     */
    operator bool() const;

    /**
     * Checks whether both objects refer to the identically the same metadata.
     */
    bool operator==(ScopedCollectionMetadata const& other) const {
        return _metadata == other._metadata;
    }
    bool operator!=(ScopedCollectionMetadata const& other) const {
        return _metadata != other._metadata;
    }


private:
    /**
     * Increments the usageCounter in the specified CollectionMetadata.
     *
     * Must be called with manager->_managerLock held.  Arguments must be non-null.
     */
    ScopedCollectionMetadata(std::shared_ptr<MetadataManager> manager,
                             std::shared_ptr<CollectionMetadata> metadata);

    /**
     * Disconnect from the CollectionMetadata, possibly triggering GC of unused CollectionMetadata.
     *
     * Must be called with manager->_managerLock held.
     */
    void _clear();

    std::shared_ptr<CollectionMetadata> _metadata{nullptr};

    std::shared_ptr<MetadataManager> _manager{nullptr};

    // These use our private ctor
    friend ScopedCollectionMetadata MetadataManager::getActiveMetadata(
        std::shared_ptr<MetadataManager>);
    friend auto MetadataManager::overlappingMetadata(std::shared_ptr<MetadataManager> const& itself,
                                                     ChunkRange const& range)
        -> std::vector<ScopedCollectionMetadata>;
};

}  // namespace mongo
