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

#include "mongo/db/repl/optime.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"

namespace mongo {

class BalancerConfiguration;
class ClusterCursorManager;
class OperationContext;
class ServiceContext;

namespace executor {
struct ConnectionPoolStats;
class NetworkInterface;
class TaskExecutorPool;
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
              std::unique_ptr<ShardRegistry> shardRegistry,
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
     * Used to indicate the sharding initialization process is complete. Should only be called once
     * in the lifetime of a server. Protected by an atomic access guard.
     */
    void setShardingInitialized();

    /**
     * If the instance as which this sharding component is running (config/shard/mongos) uses
     * additional connection pools other than the default, this function will be present and can be
     * used to obtain statistics about them. Otherwise, the value will be unset.
     */
    CustomConnectionPoolStatsFn getCustomConnectionPoolStatsFn() const;
    void setCustomConnectionPoolStatsFn(CustomConnectionPoolStatsFn statsFn);

    /**
     * Deprecated. This is only used on mongos, and once addShard is solely handled by the configs,
     * it can be deleted.
     * @return true if shards and config servers are allowed to use 'localhost' in address
     */
    bool allowLocalHost() const;

    /**
     * Deprecated. This is only used on mongos, and once addShard is solely handled by the configs,
     * it can be deleted.
     * @param whether to allow shards and config servers to use 'localhost' in address
     */
    void setAllowLocalHost(bool allow);

    ShardingCatalogClient* catalogClient() const {
        return _catalogClient.get();
    }

    CatalogCache* catalogCache() const {
        return _catalogCache.get();
    }

    ShardRegistry* shardRegistry() const {
        return _shardRegistry.get();
    }

    ClusterCursorManager* getCursorManager() const {
        return _cursorManager.get();
    }

    executor::TaskExecutorPool* getExecutorPool() const {
        return _executorPool.get();
    }

    executor::NetworkInterface* getNetwork() {
        return _network;
    }

    BalancerConfiguration* getBalancerConfiguration() const {
        return _balancerConfig.get();
    }

    /**
     * Returns the the last optime that a shard or config server has reported as the current
     * committed optime on the config server.
     * NOTE: This is not valid to call on a config server instance.
     */
    repl::OpTime configOpTime() const;

    /**
     * Called whenever a mongos or shard gets a response from a config server or shard and updates
     * what we've seen as the last config server optime.
     * If the config optime was updated, returns the previous value.
     * NOTE: This is not valid to call on a config server instance.
     */
    boost::optional<repl::OpTime> advanceConfigOpTime(OperationContext* opCtx,
                                                      repl::OpTime opTime,
                                                      StringData what);

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
    std::unique_ptr<ShardRegistry> _shardRegistry;
    std::unique_ptr<ClusterCursorManager> _cursorManager;
    std::unique_ptr<BalancerConfiguration> _balancerConfig;

    // Executor pool for scheduling work and remote commands to shards and config servers. Each
    // contained executor has a connection hook set on it for sending/receiving sharding metadata.
    std::unique_ptr<executor::TaskExecutorPool> _executorPool;

    // Network interface being used by the fixed executor in _executorPool.  Used for asking
    // questions about the network configuration, such as getting the current server's hostname.
    executor::NetworkInterface* _network{nullptr};

    CustomConnectionPoolStatsFn _customConnectionPoolStatsFn;

    AtomicWord<bool> _shardingInitialized{false};

    // Protects _configOpTime.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("Grid::_mutex");

    // Last known highest opTime from the config server that should be used when doing reads.
    // This value is updated any time a shard or mongos talks to a config server or a shard.
    repl::OpTime _configOpTime;

    /**
     * Called to update what we've seen as the last config server optime.
     * If the config optime was updated, returns the previous value.
     * NOTE: This is not valid to call on a config server instance.
     */
    boost::optional<repl::OpTime> _advanceConfigOpTime(const repl::OpTime& opTime);

    // Deprecated. This is only used on mongos, and once addShard is solely handled by the configs,
    // it can be deleted.
    // Can 'localhost' be used in shard addresses?
    bool _allowLocalShard{true};
};

}  // namespace mongo
