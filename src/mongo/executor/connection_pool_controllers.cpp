// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/connection_pool_controllers.h"

#include "mongo/util/assert_util.h"

#include <algorithm>

#include <absl/container/node_hash_map.h>
#include <fmt/format.h>

namespace mongo::executor {
namespace {
template <typename Map, typename Key>
auto& getOrInvariant(Map&& map, const Key& key) {
    auto it = map.find(key);
    invariant(it != map.end(), "Unable to find key in map");

    return it->second;
}
}  // namespace

void DynamicLimitController::init(executor::ConnectionPool* parent) {
    ControllerInterface::init(parent);
}

void DynamicLimitController::addHost(PoolId id, const HostAndPort& host) {
    std::lock_guard lk(_mutex);
    auto ret = _poolData.insert({id, {host}});
    invariant(ret.second,
              fmt::format("ConnectionPool controller {} received a request to track host {} that "
                          "was already being tracked.",
                          _name,
                          host));
}

DynamicLimitController::HostGroupState DynamicLimitController::updateHost(
    PoolId id, const PoolMetrics& stats) {
    std::lock_guard lk(_mutex);
    auto& data = getOrInvariant(_poolData, id);
    data.target =
        std::clamp(stats.requests + stats.active + stats.leased, _minLoader(), _maxLoader());
    return {{data.host}, stats.isExpired};
}

void DynamicLimitController::removeHost(PoolId id) {
    std::lock_guard lk(_mutex);
    invariant(_poolData.erase(id));
}

ConnectionPool::ConnectionControls DynamicLimitController::getControls(PoolId id) {
    std::lock_guard lk(_mutex);
    return {getPoolOptions().maxConnecting, getOrInvariant(_poolData, id).target};
}

}  // namespace mongo::executor
