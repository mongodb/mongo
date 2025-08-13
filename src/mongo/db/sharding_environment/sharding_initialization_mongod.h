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

#include "mongo/base/string_data.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/db/global_catalog/type_shard_identity.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/sharding_initialization.h"
#include "mongo/stdx/mutex.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * This class serves as a bootstrap and shutdown for the sharding subsystem and also controls the
 * persisted cluster identity. The default ShardingEnvironmentInitFunc instantiates all the sharding
 * services, attaches them to the same service context to which it itself is attached and puts the
 * ShardingState in the initialized state.
 */
class ShardingInitializationMongoD : public ReplicaSetAwareService<ShardingInitializationMongoD> {
    ShardingInitializationMongoD(const ShardingInitializationMongoD&) = delete;
    ShardingInitializationMongoD& operator=(const ShardingInitializationMongoD&) = delete;

public:
    using ShardingEnvironmentInitFunc =
        std::function<void(OperationContext* opCtx, const ShardIdentity& shardIdentity)>;

    ShardingInitializationMongoD();
    ~ShardingInitializationMongoD() override;

    static ShardingInitializationMongoD* get(OperationContext* opCtx);
    static ShardingInitializationMongoD* get(ServiceContext* service);

    /**
     * Returns the shard identity document for this shard if it exists. This method
     * will also take into account the --overrideShardIdentity startup parameter
     */
    static boost::optional<ShardIdentity> getShardIdentityDoc(OperationContext* opCtx);

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

    /**
     * Installs a listener for RSM change notifications.
     */
    void installReplicaSetChangeListener(ServiceContext* service);

private:
    void _initializeShardingEnvironmentOnShardServer(OperationContext* opCtx,
                                                     const ShardIdentity& shardIdentity);

    // Virtual methods coming from the ReplicaSetAwareService
    void onStartup(OperationContext* opCtx) final {}
    void onSetCurrentConfig(OperationContext* opCtx) final;
    void onConsistentDataAvailable(OperationContext* opCtx, bool isMajority, bool isRollback) final;
    void onShutdown() final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) final;
    void onStepUpComplete(OperationContext* opCtx, long long term) final {}
    void onStepDown() final;
    void onRollbackBegin() final {}
    void onBecomeArbiter() final {}
    inline std::string getServiceName() const final {
        return "ShardingInitializationMongoD";
    }

    AtomicWord<bool> _isPrimary;

    // This mutex ensures that only one thread at a time executes the sharding
    // initialization/teardown sequence
    stdx::mutex _initSynchronizationMutex;

    // Function for initializing the sharding environment components (i.e. everything on the Grid)
    ShardingEnvironmentInitFunc _initFunc;

    std::shared_ptr<ReplicaSetChangeNotifier::Listener> _replicaSetChangeListener;
};

/**
 * Initialize the sharding components for a mongod running as a config server (if they haven't
 * already been set up).
 */
void initializeGlobalShardingStateForConfigServer(OperationContext* opCtx);

/**
 * Helper method to initialize sharding awareness from the shard identity document if it can be
 * found and load global sharding settings awareness was initialized. See
 * ShardingInitializationMongoD::initializeShardingAwarenessIfNeeded() above for more details.
 * The optional parameter `startupTimeElapsedBuilder` is for adding time elapsed of tasks done in
 * this function into one single builder that records the time elapsed during startup. Its default
 * value is nullptr because we only want to time this function when it is called during startup.
 */
void initializeShardingAwarenessAndLoadGlobalSettings(
    OperationContext* opCtx,
    const ShardIdentity& shardIdentity,
    BSONObjBuilder* startupTimeElapsedBuilder = nullptr);

}  // namespace mongo
