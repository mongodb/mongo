// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/executor/connection_pool.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace mongo::executor {
/**
 * This file is intended for simple implementations of ConnectionPool::ControllerInterface that
 * might be shared between different libraries. Currently, it contains only one such implementation,
 * the DyamicLimitController below.
 */

/**
 * A simple controller that allows for the maximum and minimum pool size to have dynamic values.
 * At construction, provide callables that return the current maximum and minimum sizes to be used
 * by the pool.
 *
 * Currently, the callables that provide the max and min are stateless and don't inspect any data
 * about the pool. The other pool parameters (maxConnecting, host/pending/refresh timeouts) are
 * simply taken from the Optoins the relevant ConnectionPool was started with. However, this
 * type is intended to be easily extensible to add these features in the future if needed.
 */
class [[MONGO_MOD_PUBLIC]] DynamicLimitController final
    : public ConnectionPool::ControllerInterface {
public:
    DynamicLimitController(std::function<size_t()> minLoader,
                           std::function<size_t()> maxLoader,
                           std::string_view name)
        : _minLoader(std::move(minLoader)),
          _maxLoader(std::move(maxLoader)),
          _name(std::move(name)) {}

    void init(ConnectionPool* parent) override;

    void addHost(PoolId id, const HostAndPort& host) override;
    HostGroupState updateHost(PoolId id, const PoolMetrics& stats) override;
    void removeHost(PoolId id) override;

    ConnectionControls getControls(PoolId id) override;

    Milliseconds hostTimeout() const override {
        return getPoolOptions().hostTimeout;
    }

    Milliseconds pendingTimeout() const override {
        return getPoolOptions().refreshTimeout;
    }

    Milliseconds toRefreshTimeout() const override {
        return getPoolOptions().refreshRequirement;
    }

    size_t connectionRequestsMaxQueueDepth() const override {
        return getPoolOptions().connectionRequestsMaxQueueDepth;
    }

    size_t maxConnections() const override {
        return _maxLoader();
    }

    std::string_view name() const override {
        return _name;
    }

    void updateConnectionPoolStats(ConnectionPoolStats* cps) const override {}

private:
    struct PoolData {
        HostAndPort host;
        size_t target = 0;
    };

    std::function<size_t()> _minLoader;
    std::function<size_t()> _maxLoader;
    std::string _name;
    std::mutex _mutex;
    stdx::unordered_map<PoolId, PoolData> _poolData;
};
}  // namespace mongo::executor
