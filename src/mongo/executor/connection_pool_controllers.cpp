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
    stdx::lock_guard lk(_mutex);
    auto ret = _poolData.insert({id, {host}});
    invariant(ret.second,
              fmt::format("ConnectionPool controller {} received a request to track host {} that "
                          "was already being tracked.",
                          _name,
                          host));
}

DynamicLimitController::HostGroupState DynamicLimitController::updateHost(PoolId id,
                                                                          const HostState& stats) {
    stdx::lock_guard lk(_mutex);
    auto& data = getOrInvariant(_poolData, id);
    data.target =
        std::clamp(stats.requests + stats.active + stats.leased, _minLoader(), _maxLoader());
    return {{data.host}, stats.health.isExpired};
}

void DynamicLimitController::removeHost(PoolId id) {
    stdx::lock_guard lk(_mutex);
    invariant(_poolData.erase(id));
}

ConnectionPool::ConnectionControls DynamicLimitController::getControls(PoolId id) {
    stdx::lock_guard lk(_mutex);
    return {getPoolOptions().maxConnecting, getOrInvariant(_poolData, id).target};
}

}  // namespace mongo::executor
