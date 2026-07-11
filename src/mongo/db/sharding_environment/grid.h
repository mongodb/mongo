// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"

#include <functional>
#include <memory>
#include <mutex>

#include <boost/move/utility_core.hpp>

namespace mongo {

class BalancerConfiguration;
class ClusterCursorManager;

namespace executor {
class NetworkInterface;
}  // namespace executor

/**
 * Contains the sharding context for a running server. Exists on both MongoD and MongoS.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] Grid {
public:
    Grid();
    ~Grid();

    using CustomConnectionPoolStatsFn = std::function<void(executor::ConnectionPoolStats* stats)>;

    /**
     * Retrieves the instance of Grid associated with the current service/operation context.
     */
    static Grid* get(ServiceContext* serviceContext);
    static Grid* get(OperationContext* operationContext);

    /**
     * Called at startup time so the global sharding services can be set. This method must be called
     * once and once only for the lifetime of the service.
     *
     * NOTE: Unit-tests are allowed to call it more than once, provided they reset the object's
     *       state using clearForUnitTests.
     */
    void init(std::unique_ptr<ShardingCatalogClient> catalogClient,
              std::unique_ptr<CatalogCache> catalogCache,
              std::shared_ptr<ShardRegistry> shardRegistry,
              std::unique_ptr<ClusterCursorManager> cursorManager,
              std::unique_ptr<BalancerConfiguration> balancerConfig,
              std::unique_ptr<executor::TaskExecutorPool> executorPool,
              executor::NetworkInterface* network);

    /**
     * Used to check if sharding is initialized for usage of global sharding services. Protected by
     * an atomic access guard.
     */
    bool isShardingInitialized() const;

    /**
     * Throws if sharding is not initialized.
     */
    void assertShardingIsInitialized() const;

    /**
     * Used to indicate the sharding initialization process is complete. Should only be called once
     * in the lifetime of a server. Protected by an atomic access guard.
     */
    void setShardingInitialized();

    /**
     * Returns true if init() has successfully completed.
     */
    bool isInitialized() const;

    /**
     * If the instance as which this sharding component is running (config/shard/mongos) uses
     * additional connection pools other than the default, this function will be present and can be
     * used to obtain statistics about them. Otherwise, the value will be unset.
     */
    CustomConnectionPoolStatsFn getCustomConnectionPoolStatsFn() const;
    void setCustomConnectionPoolStatsFn(CustomConnectionPoolStatsFn statsFn);

    /**
     * These getter methods are safe to run only when Grid::init has been called.
     */
    ShardingCatalogClient* catalogClient() const {
        _assertInitialized();
        return _catalogClient.get();
    }

    CatalogCache* catalogCache() const {
        _assertInitialized();
        return _catalogCache.get();
    }

    ShardRegistry* shardRegistry() const {
        _assertInitialized();
        return _shardRegistry.get();
    }

    ClusterCursorManager* getCursorManager() const {
        _assertInitialized();
        return _cursorManager.get();
    }

    executor::TaskExecutorPool* getExecutorPool() const {
        _assertInitialized();
        return _executorPool.get();
    }

    executor::NetworkInterface* getNetwork() {
        _assertInitialized();
        return _network;
    }

    BalancerConfiguration* getBalancerConfiguration() const {
        _assertInitialized();
        return _balancerConfig.get();
    }

    /**
     * Called when the value of the server parameter ShardRetryTokenReturnRate changes.
     */
    static Status updateRetryBudgetReturnRate(double returnRate);

    /**
     * Called when the value of the server parameter ShardRetryTokenBucketCapacity changes.
     */
    static Status updateRetryBudgetCapacity(std::int32_t capacity);

    /**
     * Shuts down all the services that are managed by the Grid class.
     */
    void shutdown(OperationContext* opCtx,
                  BSONObjBuilder* shutdownTimeElapsedBuilder,
                  bool isMongos = false);

    /**
     * Clears the grid object so that it can be reused between test executions. This will not
     * be necessary if grid is hanging off the global ServiceContext and each test gets its
     * own service context.
     *
     * Note: shardRegistry()->shutdown() must be called before this method is called.
     *
     * NOTE: Do not use this outside of unit-tests.
     */
    void clearForUnitTests();

    /**
     * Sets the catalog cache. Used for unit-testing purposes only.
     */
    void setCatalogCache_forTest(std::unique_ptr<CatalogCache> catalogCache);

    /**
     * Sets the sharding initialization state. Used for unit-testing purposes only.
     */
    void setInitialized_forTest();

private:
    /**
     * Helper function to set retry budget server parameters on shard instances.
     */
    static Status _updateRetryBudgetRateParameters(double returnRate, double capacity);

    void _assertInitialized() const {
        uassert(ErrorCodes::ShardingStateNotInitialized,
                "Grid cannot be accessed before it is initialized",
                _isGridInitialized.load());
    }

    std::unique_ptr<ShardingCatalogClient> _catalogClient;
    std::unique_ptr<CatalogCache> _catalogCache;
    std::shared_ptr<ShardRegistry> _shardRegistry;
    std::unique_ptr<ClusterCursorManager> _cursorManager;
    std::unique_ptr<BalancerConfiguration> _balancerConfig;

    // Executor pool for scheduling work and remote commands to shards and config servers. Each
    // contained executor has a connection hook set on it for sending/receiving sharding metadata.
    std::unique_ptr<executor::TaskExecutorPool> _executorPool;

    // Network interface being used by the fixed executor in _executorPool.  Used for asking
    // questions about the network configuration, such as getting the current server's hostname.
    executor::NetworkInterface* _network{nullptr};

    Atomic<bool> _shardingInitialized{false};
    Atomic<bool> _isGridInitialized{false};

    mutable ObservableMutex<std::mutex> _mutex;

    CustomConnectionPoolStatsFn _customConnectionPoolStatsFn;
};

}  // namespace mongo
