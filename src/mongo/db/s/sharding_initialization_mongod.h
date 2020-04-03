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

#include <functional>

#include "mongo/base/string_data.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"

namespace mongo {

class ConnectionString;
class OperationContext;
class ServiceContext;

/**
 * This class serves as a bootstrap and shutdown for the sharding subsystem and also controls the
 * persisted cluster identity. The default ShardingEnvironmentInitFunc instantiates all the sharding
 * services, attaches them to the same service context to which it itself is attached and puts the
 * ShardingState in the initialized state.
 */
class ShardingInitializationMongoD {
    ShardingInitializationMongoD(const ShardingInitializationMongoD&) = delete;
    ShardingInitializationMongoD& operator=(const ShardingInitializationMongoD&) = delete;

public:
    using ShardingEnvironmentInitFunc = std::function<void(
        OperationContext* opCtx, const ShardIdentity& shardIdentity, StringData distLockProcessId)>;

    ShardingInitializationMongoD();
    ~ShardingInitializationMongoD();

    static ShardingInitializationMongoD* get(OperationContext* opCtx);
    static ShardingInitializationMongoD* get(ServiceContext* service);

    void initializeShardingEnvironmentOnShardServer(OperationContext* opCtx,
                                                    const ShardIdentity& shardIdentity,
                                                    StringData distLockProcessId);

    /**
     * If started with --shardsvr, initializes sharding awareness from the shardIdentity document on
     * disk, if there is one.
     *
     * If started with --shardsvr in queryableBackupMode, initializes sharding awareness from the
     * shardIdentity document passed through the --overrideShardIdentity startup parameter.
     *
     * If it returns true, the '_initFunc' was called, meaning all the core classes for sharding
     * were initialized, but no networking calls were made yet (with the exception of the duplicate
     * ShardRegistry reload in ShardRegistry::startup() (see SERVER-26123). Outgoing networking
     * calls to cluster members can now be made.
     *
     * If it returns false, this means the node is not yet sharding aware.
     *
     * NOTE: this function briefly takes the global lock to determine primary/secondary state.
     */
    bool initializeShardingAwarenessIfNeeded(OperationContext* opCtx);

    /**
     * Initializes the sharding state of this server from the shard identity document argument and
     * sets secondary or primary state information on the catalog cache loader.
     *
     * NOTE: This must be called under at least Global IX lock in order for the replica set member
     * state to be stable (primary/secondary).
     */
    void initializeFromShardIdentity(OperationContext* opCtx,
                                     const ShardIdentityType& shardIdentity);

    void shutDown(OperationContext* service);

    /**
     * Updates the config server field of the shardIdentity document with the given connection
     * string.
     */
    static void updateShardIdentityConfigString(OperationContext* opCtx,
                                                const ConnectionString& newConnectionString);

    /**
     * For testing only. Mock the initialization method used by initializeFromConfigConnString and
     * initializeFromShardIdentity after all checks are performed.
     */
    void setGlobalInitMethodForTest(ShardingEnvironmentInitFunc func) {
        _initFunc = std::move(func);
    }

private:
    // This mutex ensures that only one thread at a time executes the sharding
    // initialization/teardown sequence
    Mutex _initSynchronizationMutex =
        MONGO_MAKE_LATCH("ShardingInitializationMongod::_initSynchronizationMutex");

    // Function for initializing the sharding environment components (i.e. everything on the Grid)
    ShardingEnvironmentInitFunc _initFunc;

    std::shared_ptr<ReplicaSetChangeNotifier::Listener> _replicaSetChangeListener;
};

/**
 * Initialize the sharding components of this server. This can be used on both shard and config
 * servers.
 *
 * NOTE: This does not initialize ShardingState, which should only be done for shard servers.
 */
void initializeGlobalShardingStateForMongoD(OperationContext* opCtx,
                                            const ConnectionString& configCS,
                                            StringData distLockProcessId);

}  // namespace mongo
