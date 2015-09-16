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

#include <map>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/oid.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
struct ChunkVersion;
class Client;
class CollectionMetadata;
class ConnectionString;
class OperationContext;
class ServiceContext;
class Status;

namespace repl {
class OpTime;
}  // namespace repl

/**
 * Represents the sharding state for the running instance. One per instance.
 */
class ShardingState {
    MONGO_DISALLOW_COPYING(ShardingState);

public:
    ShardingState();
    ~ShardingState();

    /**
     * Retrieves the sharding state object associated with the specified service context. This
     * method must only be called if ShardingState decoration has been created on the service
     * context, otherwise it will fassert. In other words, it may only be called on MongoD and
     * tests, which specifically require and instantiate ShardingState.
     *
     * Returns the instance's ShardingState.
     */
    static ShardingState* get(ServiceContext* serviceContext);
    static ShardingState* get(OperationContext* operationContext);

    bool enabled();

    ConnectionString getConfigServer(OperationContext* txn);
    std::string getShardName();

    MigrationSourceManager* migrationSourceManager() {
        return &_migrationSourceManager;
    }

    MigrationDestinationManager* migrationDestinationManager() {
        return &_migrationDestManager;
    }

    // Initialize sharding state and begin authenticating outgoing connections and handling
    // shard versions.  If this is not run before sharded operations occur auth will not work
    // and versions will not be tracked.
    void initialize(OperationContext* txn, const std::string& server);

    // TODO: The only reason we need this method and cannot merge it together with the initialize
    // call is the setShardVersion request being sent by the config coordinator to the config server
    // instances. This is the only command, which does not include shard name and once we get rid of
    // the legacy style config servers, we can merge these methods.
    //
    // Throws an error if shard name has always been set and the newly specified value does not
    // match
    void setShardName(const std::string& shardName);

    /**
     * Clears the collection metadata cache after step down.
     */
    void clearCollectionMetadata();

    // versioning support

    bool hasVersion(const std::string& ns);
    ChunkVersion getVersion(const std::string& ns);

    /**
     * If the metadata for 'ns' at this shard is at or above the requested version,
     * 'reqShardVersion', returns OK and fills in 'latestShardVersion' with the latest shard
     * version.  The latter is always greater or equal than 'reqShardVersion' if in the same
     * epoch.
     *
     * Otherwise, falls back to refreshMetadataNow.
     *
     * This call blocks if there are more than N threads
     * currently refreshing metadata. (N is the number of
     * tickets in ShardingState::_configServerTickets,
     * currently 3.)
     *
     * Locking Note:
     *   + Must NOT be called with the write lock because this call may go into the network,
     *     and deadlocks may occur with shard-as-a-config.  Therefore, nothing here guarantees
     *     that 'latestShardVersion' is indeed the current one on return.
     */
    Status refreshMetadataIfNeeded(OperationContext* txn,
                                   const std::string& ns,
                                   const ChunkVersion& reqShardVersion,
                                   ChunkVersion* latestShardVersion);

    /**
     * Refreshes collection metadata by asking the config server for the latest information.
     * Starts a new config server request.
     *
     * Locking Notes:
     *   + Must NOT be called with the write lock because this call may go into the network,
     *     and deadlocks may occur with shard-as-a-config.  Therefore, nothing here guarantees
     *     that 'latestShardVersion' is indeed the current one on return.
     *
     *   + Because this call must not be issued with the DBLock held, by the time the config
     *     server sent us back the collection metadata information, someone else may have
     *     updated the previously stored collection metadata.  There are cases when one can't
     *     tell which of updated or loaded metadata are the freshest. There are also cases where
     *     the data coming from configs do not correspond to a consistent snapshot.
     *     In these cases, return RemoteChangeDetected. (This usually means this call needs to
     *     be issued again, at caller discretion)
     *
     * @return OK if remote metadata successfully loaded (may or may not have been installed)
     * @return RemoteChangeDetected if something changed while reloading and we may retry
     * @return !OK if something else went wrong during reload
     * @return latestShardVersion the version that is now stored for this collection
     */
    Status refreshMetadataNow(OperationContext* txn,
                              const std::string& ns,
                              ChunkVersion* latestShardVersion);

    void appendInfo(OperationContext* txn, BSONObjBuilder& b);

    // querying support

    bool needCollectionMetadata(OperationContext* txn, const std::string& ns) const;
    std::shared_ptr<CollectionMetadata> getCollectionMetadata(const std::string& ns);

    // chunk migrate and split support

    /**
     * Creates and installs a new chunk metadata for a given collection by "forgetting" about
     * one of its chunks.  The new metadata uses the provided version, which has to be higher
     * than the current metadata's shard version.
     *
     * One exception: if the forgotten chunk is the last one in this shard for the collection,
     * version has to be 0.
     *
     * If it runs successfully, clients need to grab the new version to access the collection.
     *
     * LOCKING NOTE:
     * Only safe to do inside the
     *
     * @param ns the collection
     * @param min max the chunk to eliminate from the current metadata
     * @param version at which the new metadata should be at
     */
    void donateChunk(OperationContext* txn,
                     const std::string& ns,
                     const BSONObj& min,
                     const BSONObj& max,
                     ChunkVersion version);

    /**
     * Creates and installs new chunk metadata for a given collection by reclaiming a previously
     * donated chunk.  The previous metadata's shard version has to be provided.
     *
     * If it runs successfully, clients that became stale by the previous donateChunk will be
     * able to access the collection again.
     *
     * Note: If a migration has aborted but not yet unregistered a pending chunk, replacing the
     * metadata may leave the chunk as pending - this is not dangerous and should be rare, but
     * will require a stepdown to fully recover.
     *
     * @param ns the collection
     * @param prevMetadata the previous metadata before we donated a chunk
     */
    void undoDonateChunk(OperationContext* txn,
                         const std::string& ns,
                         std::shared_ptr<CollectionMetadata> prevMetadata);

    /**
     * Remembers a chunk range between 'min' and 'max' as a range which will have data migrated
     * into it.  This data can then be protected against cleanup of orphaned data.
     *
     * Overlapping pending ranges will be removed, so it is only safe to use this when you know
     * your metadata view is definitive, such as at the start of a migration.
     *
     * @return false with errMsg if the range is owned by this shard
     */
    bool notePending(OperationContext* txn,
                     const std::string& ns,
                     const BSONObj& min,
                     const BSONObj& max,
                     const OID& epoch,
                     std::string* errMsg);

    /**
     * Stops tracking a chunk range between 'min' and 'max' that previously was having data
     * migrated into it.  This data is no longer protected against cleanup of orphaned data.
     *
     * To avoid removing pending ranges of other operations, ensure that this is only used when
     * a migration is still active.
     * TODO: Because migrations may currently be active when a collection drops, an epoch is
     * necessary to ensure the pending metadata change is still applicable.
     *
     * @return false with errMsg if the range is owned by the shard or the epoch of the metadata
     * has changed
     */
    bool forgetPending(OperationContext* txn,
                       const std::string& ns,
                       const BSONObj& min,
                       const BSONObj& max,
                       const OID& epoch,
                       std::string* errMsg);

    /**
     * Creates and installs a new chunk metadata for a given collection by splitting one of its
     * chunks in two or more. The version for the first split chunk should be provided. The
     * subsequent chunks' version would be the latter with the minor portion incremented.
     *
     * The effect on clients will depend on the version used. If the major portion is the same
     * as the current shards, clients shouldn't perceive the split.
     *
     * @param ns the collection
     * @param min max the chunk that should be split
     * @param splitKeys point in which to split
     * @param version at which the new metadata should be at
     */
    void splitChunk(OperationContext* txn,
                    const std::string& ns,
                    const BSONObj& min,
                    const BSONObj& max,
                    const std::vector<BSONObj>& splitKeys,
                    ChunkVersion version);

    /**
     * Creates and installs a new chunk metadata for a given collection by merging a range of
     * chunks ['minKey', 'maxKey') into a single chunk with version 'mergedVersion'.
     * The current metadata must overlap the range completely and minKey and maxKey must not
     * divide an existing chunk.
     *
     * The merged chunk version must have a greater version than the current shard version,
     * and if it has a greater major version clients will need to reload metadata.
     *
     * @param ns the collection
     * @param minKey maxKey the range which should be merged
     * @param newShardVersion the shard version the newly merged chunk should have
     */
    void mergeChunks(OperationContext* txn,
                     const std::string& ns,
                     const BSONObj& minKey,
                     const BSONObj& maxKey,
                     ChunkVersion mergedVersion);

    bool inCriticalMigrateSection();

    /**
     * @return true if we are NOT in the critical section
     */
    bool waitTillNotInCriticalSection(int maxSecondsToWait);

    /**
     * TESTING ONLY
     * Uninstalls the metadata for a given collection.
     */
    void resetMetadata(const std::string& ns);

private:
    // Map from a namespace into the metadata we need for each collection on this shard
    typedef std::map<std::string, std::shared_ptr<CollectionMetadata>> CollectionMetadataMap;

    /**
     * Refreshes collection metadata by asking the config server for the latest information.
     * May or may not be based on a requested version.
     */
    Status doRefreshMetadata(OperationContext* txn,
                             const std::string& ns,
                             const ChunkVersion& reqShardVersion,
                             bool useRequestedVersion,
                             ChunkVersion* latestShardVersion);

    // Manages the state of the migration donor shard
    MigrationSourceManager _migrationSourceManager;

    // Manages the state of the migration recipient shard
    MigrationDestinationManager _migrationDestManager;

    // Protects state below
    stdx::mutex _mutex;

    // Whether ::initialize has been called
    bool _enabled;

    // Sets the shard name for this host (comes through setShardVersion)
    std::string _shardName;

    // protects accessing the config server
    // Using a ticket holder so we can have multiple redundant tries at any given time
    mutable TicketHolder _configServerTickets;

    CollectionMetadataMap _collMetadata;
};

}  // namespace mongo
