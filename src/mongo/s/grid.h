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
#include <memory>

#include <boost/move/utility_core.hpp>

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/util/assert_util_core.h"

namespace mongo {

class BalancerConfiguration;
class ClusterCursorManager;

namespace executor {
class NetworkInterface;
}  // namespace executor

/**
 * Contains the sharding context for a running server. Exists on both MongoD and MongoS.
 */
class Grid {
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
        dassert(_isGridInitialized.load());
        return _catalogClient.get();
    }

    CatalogCache* catalogCache() const {
        dassert(_isGridInitialized.load());
        return _catalogCache.get();
    }

    ShardRegistry* shardRegistry() const {
        dassert(_isGridInitialized.load());
        return _shardRegistry.get();
    }

    ClusterCursorManager* getCursorManager() const {
        dassert(_isGridInitialized.load());
        return _cursorManager.get();
    }

    executor::TaskExecutorPool* getExecutorPool() const {
        dassert(_isGridInitialized.load());
        return _executorPool.get();
    }

    executor::NetworkInterface* getNetwork() {
        dassert(_isGridInitialized.load());
        return _network;
    }

    BalancerConfiguration* getBalancerConfiguration() const {
        dassert(_isGridInitialized.load());
        return _balancerConfig.get();
    }

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

private:
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

    AtomicWord<bool> _shardingInitialized{false};
    AtomicWord<bool> _isGridInitialized{false};

    mutable Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "Grid::_mutex");

    CustomConnectionPoolStatsFn _customConnectionPoolStatsFn;
};

}  // namespace mongo
