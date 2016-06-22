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
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
struct ChunkVersion;
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

    bool enabled() const;

    ConnectionString getConfigServer(OperationContext* txn);

    std::string getShardName();

    MigrationDestinationManager* migrationDestinationManager() {
        return &_migrationDestManager;
    }

    /**
     * Initializes sharding state and begins authenticating outgoing connections and handling shard
     * versions. If this is not run before sharded operations occur auth will not work and versions
     * will not be tracked. This method is deprecated and is mainly used for initialization from
     * mongos metadata commands like moveChunk, splitChunk, mergeChunk and setShardVersion.
     *
     * Throws if initialization fails for any reason and the sharding state object becomes unusable
     * afterwards. Any sharding state operations afterwards will fail.
     *
     * Note that this will also try to connect to the config servers and will block until it
     * succeeds.
     */
    void initializeFromConfigConnString(OperationContext* txn, const std::string& configSvr);

    /**
     * Initializes the sharding state of this server from the shard identity document from local
     * storage.
     *
     * Note that this will also try to connect to the config servers and will block until it
     * succeeds.
     */
    Status initializeFromShardIdentity(OperationContext* txn);

    /**
     * Initializes the sharding state of this server from the shard identity document argument.
     * This is the more genaralized form of the initializeFromShardIdentity(OperationContext*)
     * method that can accept the shard identity from any source. Note that shardIdentity must
     * be valid.
     *
     * Returns ErrorCodes::ExceededTimeLimit if deadline has passed.
     */
    Status initializeFromShardIdentity(OperationContext* txn,
                                       const ShardIdentityType& shardIdentity,
                                       Date_t deadline);

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

    ScopedCollectionMetadata getCollectionMetadata(const std::string& ns);

    /**
     * Updates the config server field of the shardIdentity document with the given connection
     * string.
     *
     * Note: this can return NotMaster error.
     */
    Status updateShardIdentityConfigString(OperationContext* txn,
                                           const std::string& newConnectionString);

    /**
     * TESTING ONLY
     * Uninstalls the metadata for a given collection.
     */
    void resetMetadata(const std::string& ns);

    /**
     * If there are no migrations running on this shard, registers an active migration with the
     * specified arguments and returns a ScopedRegisterMigration, which must be signaled by the
     * caller before it goes out of scope.
     *
     * If there is an active migration already running on this shard and it has the exact same
     * arguments, returns a ScopedRegisterMigration, which can be used to join the existing one.
     *
     * Othwerwise returns a ConflictingOperationInProgress error.
     */
    StatusWith<ScopedRegisterMigration> registerMigration(const MoveChunkRequest& args);

    /**
     * If a migration has been previously registered through a call to registerMigration returns
     * that namespace. Otherwise returns boost::none.
     *
     * This method can be called without any locks, but once the namespace is fetched it needs to be
     * re-checked after acquiring some intent lock on that namespace.
     */
    boost::optional<NamespaceString> getActiveMigrationNss();

    /**
     * For testing only. Mock the initialization method used by initializeFromConfigConnString and
     * initializeFromShardIdentity after all checks are performed.
     */
    void setGlobalInitMethodForTest(GlobalInitFunc func);

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
    Status _waitForInitialization(Date_t deadline);
    Status _waitForInitialization_inlock(Date_t deadline, stdx::unique_lock<stdx::mutex>& lk);

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

    // Manages the state of the migration recipient shard
    MigrationDestinationManager _migrationDestManager;

    // Tracks the active move chunk operations running on this shard
    ActiveMigrationsRegistry _activeMigrationsRegistry;

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

    // Cache of collection metadata on this shard. It is not safe to look-up values from this map
    // without holding some form of collection lock. It is only safe to add/remove values when
    // holding X lock on the respective namespace.
    CollectionShardingStateMap _collections;

    // The id for the cluster this shard belongs to.
    OID _clusterId;

    // Function for initializing the external sharding state components not owned here.
    GlobalInitFunc _globalInit;
};

}  // namespace mongo
