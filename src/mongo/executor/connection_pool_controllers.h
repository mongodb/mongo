/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <functional>
#include <string>
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
class DynamicLimitController final : public ConnectionPool::ControllerInterface {
public:
    DynamicLimitController(std::function<size_t()> minLoader,
                           std::function<size_t()> maxLoader,
                           StringData name)
        : _minLoader(std::move(minLoader)),
          _maxLoader(std::move(maxLoader)),
          _name(std::move(name)) {}

    void init(ConnectionPool* parent) override;

    void addHost(PoolId id, const HostAndPort& host) override;
    HostGroupState updateHost(PoolId id, const HostState& stats) override;
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

    StringData name() const override {
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
    stdx::mutex _mutex;
    stdx::unordered_map<PoolId, PoolData> _poolData;
};
}  // namespace mongo::executor
