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

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/oid.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/chunk_splitter.h"
#include "mongo/db/s/collection_range_deleter.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
struct ChunkVersion;
class CollectionMetadata;
class CollectionShardingState;
class ConnectionString;
class OperationContext;
class ScopedCollectionMetadata;
class ServiceContext;
class ShardIdentityType;
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
    using GlobalInitFunc =
        stdx::function<Status(OperationContext*, const ConnectionString&, StringData)>;

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

    /**
     * Returns true if ShardingState has been successfully initialized.
     *
     * Code that needs to perform extra actions if sharding is initialized, but does not need to
     * error if not, should use this. Alternatively, see ShardingState::canAcceptShardedCommands().
     */
    bool enabled() const;

    /**
     * Force-sets the initialization state to InitializationState::kInitialized, for testing
     * purposes. Note that this function should ONLY be used for testing purposes.
     */
    void setEnabledForTest(const std::string& shardName);

    /**
     * Returns Status::OK if the ShardingState is enabled; if not, returns an error describing
     * whether the ShardingState is just not yet initialized, or if this shard is not running with
     * --shardsvr at all.
     *
     * Code that should error if sharding state has not been initialized should use this to report
     * a more descriptive error. Alternatively, see ShardingState::enabled().
     */
    Status canAcceptShardedCommands() const;

    ConnectionString getConfigServer(OperationContext* opCtx);

    std::string getShardName();

    MigrationDestinationManager* migrationDestinationManager() {
        return &_migrationDestManager;
    }

    /**
     * Initializes the sharding state of this server from the shard identity document argument
     * and sets secondary or primary state information on the catalog cache loader.
     *
     * Note: caller must hold a global/database lock! Needed in order to stably check for
     * replica set state (primary, secondary, standalone).
     */
    Status initializeFromShardIdentity(OperationContext* opCtx,
                                       const ShardIdentityType& shardIdentity);

    /**
     * Shuts down sharding machinery on the shard.
     */
    void shutDown(OperationContext* opCtx);

    /**
     * Updates the ShardRegistry's stored notion of the config server optime based on the
     * ConfigServerMetadata decoration attached to the OperationContext.
     */
    Status updateConfigServerOpTimeFromMetadata(OperationContext* opCtx);

    CollectionShardingState* getNS(const std::string& ns, OperationContext* opCtx);

    /**
     * Should be invoked when the shard server primary enters the 'PRIMARY' state.
     * Sets up the ChunkSplitter to begin accepting split requests.
     */
    void initiateChunkSplitter();

    /**
     * Should be invoked when this node which is currently serving as a 'PRIMARY' steps down.
     * Sets the state of the ChunkSplitter so that it will no longer accept split requests.
     */
    void interruptChunkSplitter();

    /**
     * Iterates through all known sharded collections and marks them (in memory only) as not sharded
     * so that no filtering will be happening for slaveOk queries.
     */
    void markCollectionsNotShardedAtStepdown();

    /**
     * Refreshes the local metadata based on whether the expected version is higher than what we
     * have cached.
     */
    Status onStaleShardVersion(OperationContext* opCtx,
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
    Status refreshMetadataNow(OperationContext* opCtx,
                              const NamespaceString& nss,
                              ChunkVersion* latestShardVersion);

    void appendInfo(OperationContext* opCtx, BSONObjBuilder& b);

    bool needCollectionMetadata(OperationContext* opCtx, const std::string& ns);

    /**
     * Updates the config server field of the shardIdentity document with the given connection
     * string.
     *
     * Note: this can return NotMaster error.
     */
    Status updateShardIdentityConfigString(OperationContext* opCtx,
                                           const std::string& newConnectionString);

    /**
     * If there are no migrations running on this shard, registers an active migration with the
     * specified arguments and returns a ScopedRegisterDonateChunk, which must be signaled by the
     * caller before it goes out of scope.
     *
     * If there is an active migration already running on this shard and it has the exact same
     * arguments, returns a ScopedRegisterDonateChunk, which can be used to join the existing one.
     *
     * Othwerwise returns a ConflictingOperationInProgress error.
     */
    StatusWith<ScopedRegisterDonateChunk> registerDonateChunk(const MoveChunkRequest& args);

    /**
     * If there are no migrations running on this shard, registers an active receive operation with
     * the specified session id and returns a ScopedRegisterReceiveChunk, which will unregister it
     * when it goes out of scope.
     *
     * Otherwise returns a ConflictingOperationInProgress error.
     */
    StatusWith<ScopedRegisterReceiveChunk> registerReceiveChunk(const NamespaceString& nss,
                                                                const ChunkRange& chunkRange,
                                                                const ShardId& fromShardId);

    /**
     * If a migration has been previously registered through a call to registerDonateChunk returns
     * that namespace. Otherwise returns boost::none.
     *
     * This method can be called without any locks, but once the namespace is fetched it needs to be
     * re-checked after acquiring some intent lock on that namespace.
     */
    boost::optional<NamespaceString> getActiveDonateChunkNss();

    /**
     * Get a migration status report from the migration registry. If no migration is active, this
     * returns an empty BSONObj.
     *
     * Takes an IS lock on the namespace of the active migration, if one is active.
     */
    BSONObj getActiveMigrationStatusReport(OperationContext* opCtx);

    /**
     * For testing only. Mock the initialization method used by initializeFromConfigConnString and
     * initializeFromShardIdentity after all checks are performed.
     */
    void setGlobalInitMethodForTest(GlobalInitFunc func);

    /**
     * Schedules for the range to clean of the given namespace to be deleted.
     * Behavior can be modified through setScheduleCleanupFunctionForTest.
     */
    void scheduleCleanup(const NamespaceString& nss);

    /**
     * If started with --shardsvr, initializes sharding awareness from the shardIdentity document
     * on disk, if there is one.
     * If started with --shardsvr in queryableBackupMode, initializes sharding awareness from the
     * shardIdentity document passed through the --overrideShardIdentity startup parameter.
     *
     * If returns true, the ShardingState::_globalInit method was called, meaning all the core
     * classes for sharding were initialized, but no networking calls were made yet (with the
     * exception of the duplicate ShardRegistry reload in ShardRegistry::startup() (see
     * SERVER-26123). Outgoing networking calls to cluster members can now be made.
     *
     * Note: this function briefly takes the global lock to determine primary/secondary state.
     */
    StatusWith<bool> initializeShardingAwarenessIfNeeded(OperationContext* opCtx);

    /**
     * Return the task executor to be shared by the range deleters for all collections.
     */
    executor::TaskExecutor* getRangeDeleterTaskExecutor();

private:
    // Map from a namespace into the sharding state for each collection we have
    typedef stdx::unordered_map<std::string, std::unique_ptr<CollectionShardingState>>
        CollectionShardingStateMap;

    // Progress of the sharding state initialization
    enum class InitializationState : uint32_t {
        // Initial state. The server must be under exclusive lock when this state is entered. No
        // metadata is available yet and it is not known whether there is any min optime metadata,
        // which needs to be recovered. From this state, the server may enter INITIALIZING, if a
        // recovey document is found or stay in it until initialize has been called.
        kNew,

        // Sharding state is fully usable.
        kInitialized,

        // Some initialization error occurred. The _initializationStatus variable will contain the
        // error.
        kError,
    };

    /**
     * Returns the initialization state.
     */
    InitializationState _getInitializationState() const;

    /**
     * Updates the initialization state.
     */
    void _setInitializationState(InitializationState newState);

    /**
     * Refreshes collection metadata by asking the config server for the latest information and
     * returns the latest version at the time the reload was done. This call does network I/O and
     * should never be called with a lock.
     */
    ChunkVersion _refreshMetadata(OperationContext* opCtx, const NamespaceString& nss);

    // Manages the state of the migration recipient shard
    MigrationDestinationManager _migrationDestManager;

    // Tracks the active move chunk operations running on this shard
    ActiveMigrationsRegistry _activeMigrationsRegistry;

    // Handles asynchronous auto-splitting of chunks
    std::unique_ptr<ChunkSplitter> _chunkSplitter;

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

    // Cache of collection metadata on this shard. It is not safe to look-up values from this map
    // without holding some form of collection lock. It is only safe to add/remove values when
    // holding X lock on the respective namespace.
    CollectionShardingStateMap _collections;

    // The id for the cluster this shard belongs to.
    OID _clusterId;

    // Function for initializing the external sharding state components not owned here.
    GlobalInitFunc _globalInit;

    // Task executor shared by the collection range deleters.
    struct RangeDeleterExecutor {
        stdx::mutex lock{};
        std::unique_ptr<executor::TaskExecutor> taskExecutor{nullptr};
        ~RangeDeleterExecutor();
    };
    RangeDeleterExecutor _rangeDeleterExecutor;
};

}  // namespace mongo
