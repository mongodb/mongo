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
#include "mongo/db/namespace_string.h"
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
class CollectionShardingState;
class ConnectionString;
class OperationContext;
class ServiceContext;
class Status;

namespace repl {
class OpTime;
}  // namespace repl

/**
 * Contains the global sharding state for a running mongod. There is one instance of this object per
 * service context and it is never destroyed for the lifetime of the context.
 */
class ShardingState {
    MONGO_DISALLOW_COPYING(ShardingState);

public:
    /**
     * RAII object, which will register an active migration with the global sharding state so that
     * no subsequent migrations may start until the previous one has completed.
     */
    class ScopedRegisterMigration {
        MONGO_DISALLOW_COPYING(ScopedRegisterMigration);

    public:
        /**
         * Registers a new migration with the global sharding state. If a migration is already
         * active it will throw a user assertion with a ConflictingOperationInProgress code.
         */
        ScopedRegisterMigration(OperationContext* txn, NamespaceString nss);
        ~ScopedRegisterMigration();

    private:
        // The operation context under which we are running. Must remain the same.
        OperationContext* const _txn;
    };

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

    bool enabled() const;

    ConnectionString getConfigServer(OperationContext* txn);

    std::string getShardName();

    MigrationSourceManager* migrationSourceManager() {
        return &_migrationSourceManager;
    }

    MigrationDestinationManager* migrationDestinationManager() {
        return &_migrationDestManager;
    }

    /**
     * Initializes sharding state and begins authenticating outgoing connections and handling shard
     * versions. If this is not run before sharded operations occur auth will not work and versions
     * will not be tracked.
     *
     * Throws if initialization fails for any reason and the sharding state object becomes unusable
     * afterwards. Any sharding state operations afterwards will fail.
     */
    void initialize(OperationContext* txn, const std::string& configSvr);

    /**
     * Shuts down sharding machinery on the shard.
     */
    void shutDown(OperationContext* txn);

    /**
     * Updates the ShardRegistry's stored notion of the config server optime based on the
     * ConfigServerMetadata decoration attached to the OperationContext.
     */
    void updateConfigServerOpTimeFromMetadata(OperationContext* txn);

    /**
     * Assigns a shard name to this MongoD instance.
     * TODO: The only reason we need this method and cannot merge it together with the initialize
     * call is the setShardVersion request being sent by the config coordinator to the config server
     * instances. This is the only command, which does not include shard name and once we get rid of
     * the legacy style config servers, we can merge these methods.
     *
     * Throws an error if shard name has always been set and the newly specified value does not
     * match what was previously installed.
     */
    void setShardName(const std::string& shardName);

    CollectionShardingState* getNS(const std::string& ns);

    /**
     * Clears the collection metadata cache after step down.
     */
    void clearCollectionMetadata();

    ChunkVersion getVersion(const std::string& ns);

    /**
     * Refreshes the local metadata based on whether the expected version is higher than what we
     * have cached.
     */
    Status onStaleShardVersion(OperationContext* txn,
                               const NamespaceString& nss,
                               const ChunkVersion& expectedVersion);

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

    bool needCollectionMetadata(OperationContext* txn, const std::string& ns);

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

    /**
     * TESTING ONLY
     * Uninstalls the metadata for a given collection.
     */
    void resetMetadata(const std::string& ns);

    /**
     * If a migration has been previously registered through a call to registerMigration returns
     * that namespace. Otherwise returns boost::none.
     *
     * This method can be called without any locks, but once the namespace is fetched it needs to be
     * re-checked after acquiring some intent lock on that namespace.
     */
    boost::optional<NamespaceString> getActiveMigrationNss();

private:
    friend class ScopedRegisterMigration;

    // Map from a namespace into the sharding state for each collection we have
    typedef std::map<std::string, std::unique_ptr<CollectionShardingState>>
        CollectionShardingStateMap;

    // Progress of the sharding state initialization
    enum class InitializationState : uint32_t {
        // Initial state. The server must be under exclusive lock when this state is entered. No
        // metadata is available yet and it is not known whether there is any min optime metadata,
        // which needs to be recovered. From this state, the server may enter INITIALIZING, if a
        // recovey document is found or stay in it until initialize has been called.
        kNew,

        // The sharding state has been recovered (or doesn't need to be recovered) and the catalog
        // manager is currently being initialized by one of the threads.
        kInitializing,

        // Sharding state is fully usable.
        kInitialized,

        // Some initialization error occurred. The _initializationStatus variable will contain the
        // error.
        kError,
    };

    /**
     * Initializes the sharding infrastructure (connection hook, catalog manager, etc) and
     * optionally recovers its minimum optime. Must not be called while holding the sharding state
     * mutex.
     *
     * Doesn't throw, only updates the initialization state variables.
     *
     * Runs in a new thread so that if all config servers are down initialization can continue
     * retrying in the background even if the operation that kicked off the initialization has
     * terminated.
     *
     * @param configSvr Connection string of the config server to use.
     */
    void _initializeImpl(ConnectionString configSvr);

    /**
     * Must be called only when the current state is kInitializing. Sets the current state to
     * kInitialized if the status is OK or to kError otherwise.
     */
    void _signalInitializationComplete(Status status);

    /**
     * Blocking method, which waits for the initialization state to become kInitialized or kError
     * and returns the initialization status.
     */
    Status _waitForInitialization(OperationContext* txn);

    /**
     * Simple wrapper to cast the initialization state atomic uint64 to InitializationState value
     * without doing any locking.
     */
    InitializationState _getInitializationState() const;

    /**
     * Updates the initialization state. Must be called while holding _mutex.
     */
    void _setInitializationState_inlock(InitializationState newState);

    /**
     * Refreshes collection metadata by asking the config server for the latest information. May or
     * may not be based on a requested version.
     */
    Status _refreshMetadata(OperationContext* txn,
                            const std::string& ns,
                            const ChunkVersion& reqShardVersion,
                            bool useRequestedVersion,
                            ChunkVersion* latestShardVersion);

    /**
     * Registers a namespace with ongoing migration. This is what ensures that there is a single
     * migration active per shard.
     *
     * Returns OK if there is no active migration and ConflictingOperationInProgress otherwise.
     */
    Status _registerMigration(NamespaceString nss);

    /**
     * Unregisters a previously registered namespace with ongoing migration. Must only be called if
     * a previous call to registerMigration has succeeded.
     */
    void _clearMigration();

    // Manages the state of the migration donor shard
    MigrationSourceManager _migrationSourceManager;

    // Manages the state of the migration recipient shard
    MigrationDestinationManager _migrationDestManager;

    // Protects state below
    stdx::mutex _mutex;

    // State of the initialization of the sharding state along with any potential errors
    AtomicUInt32 _initializationState;

    // Only valid if _initializationState is kError. Contains the reason for initialization failure.
    Status _initializationStatus;

    // Signaled when ::initialize finishes.
    stdx::condition_variable _initializationFinishedCondition;

    // Sets the shard name for this host (comes through setShardVersion)
    std::string _shardName;

    // Protects from hitting the config server from too many threads at once
    TicketHolder _configServerTickets;

    // If there is an active migration going on, this field contains the namespace which is being
    // migrated. The need for this is due to the fact that _initialClone/_transferMods do not carry
    // any namespace with them. This value can be read using only the global sharding state mutex
    // (_mutex), but to be set requires both collection lock and the mutex.
    boost::optional<NamespaceString> _activeMigrationNss;

    // Cache of collection metadata on this shard. It is not safe to look-up values from this map
    // without holding some form of collection lock. It is only safe to add/remove values when
    // holding X lock on the respective namespace.
    CollectionShardingStateMap _collections;
};

}  // namespace mongo
