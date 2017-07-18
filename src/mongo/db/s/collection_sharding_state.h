/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/collection_range_deleter.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/util/concurrency/notification.h"

namespace mongo {

// How long to wait before starting cleanup of an emigrated chunk range.
extern AtomicInt32 orphanCleanupDelaySecs;

class BalancerConfiguration;
class BSONObj;
struct ChunkVersion;
class CollectionMetadata;
class MigrationSourceManager;
class OperationContext;

/**
 * Contains all sharding-related runtime state for a given collection. One such object is assigned
 * to each sharded collection known on a mongod instance. A set of these objects is linked off the
 * instance's sharding state.
 *
 * Synchronization rules: In order to look-up this object in the instance's sharding map, one must
 * have some lock on the respective collection.
 */
class CollectionShardingState {
    MONGO_DISALLOW_COPYING(CollectionShardingState);

public:
    using CleanupNotification = CollectionRangeDeleter::DeleteNotification;

    /**
     * Instantiates a new per-collection sharding state as unsharded.
     */
    CollectionShardingState(ServiceContext* sc, NamespaceString nss);
    ~CollectionShardingState();

    /**
     * Holds information used for tracking document removals during chunk migration.
     */
    struct DeleteState {
        // Contains the _id field of the document being deleted.
        BSONObj idDoc;

        // True if the document being deleted belongs to a chunk which is currently being migrated
        // out of this shard.
        bool isMigrating = false;
    };

    /**
     * Obtains the sharding state for the specified collection. If it does not exist, it will be
     * created and will remain active until the collection is dropped or unsharded.
     *
     * Must be called with some lock held on the specific collection being looked up and the
     * returned pointer should never be stored.
     */
    static CollectionShardingState* get(OperationContext* opCtx, const NamespaceString& nss);
    static CollectionShardingState* get(OperationContext* opCtx, const std::string& ns);

    /**
     * Returns the chunk metadata for the collection. The metadata it represents lives as long as
     * the object itself, and the collection, exist. After dropping the collection lock, the
     * collection may no longer exist, but it is still safe to destroy the object.
     */
    ScopedCollectionMetadata getMetadata();

    /**
     * BSON output of the pending metadata into a BSONArray
     */
    void toBSONPending(BSONArrayBuilder& bb) const {
        _metadataManager->toBSONPending(bb);
    }

    /**
     * Updates the metadata based on changes received from the config server and also resolves the
     * pending receives map in case some of these pending receives have completed or have been
     * abandoned.  If newMetadata is null, unshard the collection.
     *
     * Must always be called with an exclusive collection lock.
     */
    void refreshMetadata(OperationContext* opCtx, std::unique_ptr<CollectionMetadata> newMetadata);

    /**
     * Marks the collection as not sharded at stepdown time so that no filtering will occur for
     * slaveOk queries.
     */
    void markNotShardedAtStepdown();

    /**
     * Schedules any documents in `range` for immediate cleanup iff no running queries can depend
     * on them, and adds the range to the list of pending ranges. Otherwise, returns a notification
     * that yields bad status immediately.  Does not block.  Call waitStatus(opCtx) on the result
     * to wait for the deletion to complete or fail.  After that, call waitForClean to ensure no
     * other deletions are pending for the range.
     */
    auto beginReceive(ChunkRange const& range) -> CleanupNotification;

    /*
     * Removes `range` from the list of pending ranges, and schedules any documents in the range for
     * immediate cleanup.  Does not block.
     */
    void forgetReceive(const ChunkRange& range);

    /**
     * Schedules documents in `range` for cleanup after any running queries that may depend on them
     * have terminated. Does not block. Fails if range overlaps any current local shard chunk.
     * Passed kDelayed, an additional delay (configured via server parameter orphanCleanupDelaySecs)
     * is added to permit (most) dependent queries on secondaries to complete, too.
     *
     * Call result.waitStatus(opCtx) to wait for the deletion to complete or fail. If that succeeds,
     * waitForClean can be called to ensure no other deletions are pending for the range. Call
     * result.abandon(), instead of waitStatus, to ignore the outcome.
     */
    enum CleanWhen { kNow, kDelayed };
    auto cleanUpRange(ChunkRange const& range, CleanWhen) -> CleanupNotification;

    /**
     * Returns a vector of ScopedCollectionMetadata objects representing metadata instances in use
     * by running queries that overlap the argument range, suitable for identifying and invalidating
     * those queries.
     */
    auto overlappingMetadata(ChunkRange const& range) const
        -> std::vector<ScopedCollectionMetadata>;

    /**
     * Returns the active migration source manager, if one is available.
     */
    MigrationSourceManager* getMigrationSourceManager();

    /**
     * Attaches a migration source manager to this collection's sharding state. Must be called with
     * collection X lock. May not be called if there is a migration source manager already
     * installed. Must be followed by a call to clearMigrationSourceManager.
     */
    void setMigrationSourceManager(OperationContext* opCtx, MigrationSourceManager* sourceMgr);

    /**
     * Removes a migration source manager from this collection's sharding state. Must be called with
     * collection X lock. May not be called if there isn't a migration source manager installed
     * already through a previous call to setMigrationSourceManager.
     */
    void clearMigrationSourceManager(OperationContext* opCtx);

    /**
     * Checks whether the shard version in the context is compatible with the shard version of the
     * collection locally and if not throws SendStaleConfigException populated with the expected and
     * actual versions.
     *
     * Because SendStaleConfigException has special semantics in terms of how a sharded command's
     * response is constructed, this function should be the only means of checking for shard version
     * match.
     */
    void checkShardVersionOrThrow(OperationContext* opCtx);

    /**
     * Returns whether this collection is sharded. Valid only if mongoD is primary.
     * TODO SERVER-24960: This method may return a false positive until SERVER-24960 is fixed.
     */
    bool collectionIsSharded();

    /**
     * Tracks deletion of any documents within the range, returning when deletion is complete.
     * Throws if the collection is dropped while it sleeps. Call this with the collection unlocked.
     */
    static Status waitForClean(OperationContext*, NamespaceString, OID const& epoch, ChunkRange);

    /**
     * Reports whether any range still scheduled for deletion overlaps the argument range. If so,
     * it returns a notification n such that n->get(opCtx) will wake when the newest overlapping
     * range's deletion (possibly the one of interest) completes or fails. This should be called
     * again after each wakeup until it returns boost::none, because there can be more than one
     * range scheduled for deletion that overlaps its argument.
     */
    auto trackOrphanedDataCleanup(ChunkRange const& range) -> boost::optional<CleanupNotification>;

    /**
     * Returns a range _not_ owned by this shard that starts no lower than the specified
     * startingFrom key value, if any, or boost::none if there is no such range.
     */
    boost::optional<KeyRange> getNextOrphanRange(BSONObj const& startingFrom);

    /**
     * Replication oplog OpObserver hooks. Informs the sharding system of changes that may be
     * relevant to ongoing operations.
     *
     * The global exclusive lock is expected to be held by the caller of any of these functions.
     */
    bool isDocumentInMigratingChunk(OperationContext* opCtx, const BSONObj& doc);
    void onInsertOp(OperationContext* opCtx, const BSONObj& insertedDoc);
    void onUpdateOp(OperationContext* opCtx,
                    const BSONObj& query,
                    const BSONObj& update,
                    const BSONObj& updatedDoc);
    void onDeleteOp(OperationContext* opCtx, const DeleteState& deleteState);
    void onDropCollection(OperationContext* opCtx, const NamespaceString& collectionName);

private:
    /**
     * Registers a task on the opCtx -- to run after writes from the oplog are committed and visible
     * to reads -- to notify the catalog cache loader of a new collection version. The catalog
     * cache's routing table for the collection will also be invalidated at that time so that the
     * next caller to the catalog cache for routing information will provoke a routing table
     * refresh.
     *
     * This only runs on secondaries, and only when 'lastRefreshedCollectionVersion' is in 'update',
     * meaning a chunk metadata refresh finished being applied to the collection's locally persisted
     * metadata store.
     *
     * query - BSON with an _id that identifies which collections entry is being updated.
     * update - the update being applied to the collections entry.
     * updatedDoc - the document identified by 'query' with the 'update' applied.
     *
     * The global exclusive lock is expected to be held by the caller.
     */
    void _onConfigRefreshCompleteInvalidateCachedMetadataAndNotify(OperationContext* opCtx,
                                                                   const BSONObj& query,
                                                                   const BSONObj& update,
                                                                   const BSONObj& updatedDoc);

    /**
     * Registers a task on the opCtx -- to run after writes from the oplog are committed and visible
     * to reads -- to notify the catalog cache loader of a new collection version. The catalog
     * cache's routing table for the collection will also be invalidated at that time so that the
     * next caller to the catalog cache for routing information will provoke a routing table
     * refresh.
     *
     * This only runs on secondaries
     *
     * query - BSON with an _id field that identifies which collections entry is being updated.
     *
     * The global exclusive lock is expected to be held by the caller.
     */
    void _onConfigDeleteInvalidateCachedMetadataAndNotify(OperationContext* opCtx,
                                                          const BSONObj& query);

    /**
     * Checks whether the shard version of the operation matches that of the collection.
     *
     * opCtx - Operation context from which to retrieve the operation's expected version.
     * errmsg (out) - On false return contains an explanatory error message.
     * expectedShardVersion (out) - On false return contains the expected collection version on this
     *  shard. Obtained from the operation sharding state.
     * actualShardVersion (out) - On false return contains the actual collection version on this
     *  shard. Obtained from the collection sharding state.
     *
     * Returns true if the expected collection version on the shard matches its actual version on
     * the shard and false otherwise. Upon false return, the output parameters will be set.
     */
    bool _checkShardVersionOk(OperationContext* opCtx,
                              std::string* errmsg,
                              ChunkVersion* expectedShardVersion,
                              ChunkVersion* actualShardVersion);

    /**
     * If the collection is sharded, finds the chunk that contains the specified document, and
     * increments the size tracked for that chunk by the specified amount of data written, in
     * bytes. Returns the number of total bytes on that chunk, after the data is written.
     */
    uint64_t _incrementChunkOnInsertOrUpdate(OperationContext* opCtx,
                                             const BSONObj& document,
                                             long dataWritten);

    /**
     * Returns true if the total number of bytes on the specified chunk nears the max size of
     * a shard.
     */
    bool _shouldSplitChunk(OperationContext* opCtx,
                           const ShardKeyPattern& shardKeyPattern,
                           const Chunk& chunk);

    // Namespace this state belongs to.
    const NamespaceString _nss;

    // Contains all the metadata associated with this collection.
    std::shared_ptr<MetadataManager> _metadataManager;

    // If this collection is serving as a source shard for chunk migration, this value will be
    // non-null. To write this value there needs to be X-lock on the collection in order to
    // synchronize with other callers, which read it.
    //
    // NOTE: The value is not owned by this class.
    MigrationSourceManager* _sourceMgr{nullptr};

    // for access to _metadataManager
    friend auto CollectionRangeDeleter::cleanUpNextRange(OperationContext*,
                                                         NamespaceString const&,
                                                         OID const& epoch,
                                                         int maxToDelete,
                                                         CollectionRangeDeleter*)
        -> boost::optional<Date_t>;
};

}  // namespace mongo
