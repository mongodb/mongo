/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/client/replica_set_monitor.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/sharding_task_executor_pool_controller.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kConnectionPool


namespace mongo {

namespace {

template <typename Map, typename Key>
auto& getOrInvariant(Map&& map, const Key& key) noexcept {
    auto it = std::forward<Map>(map).find(key);
    invariant(it != std::forward<Map>(map).end(), "Unable to find key in map");

    return it->second;
}

template <typename Map, typename... Args>
void emplaceOrInvariant(Map&& map, Args&&... args) noexcept {
    auto ret = std::forward<Map>(map).emplace(std::forward<Args>(args)...);
    invariant(ret.second, "Element already existed in map/set");
}

bool isConfigServer(const ShardRegistry* sr, const HostAndPort& peer) {
    return sr && sr->isConfigServer(peer);
}

}  // namespace

Status ShardingTaskExecutorPoolController::validateHostTimeout(const int& hostTimeoutMS) {
    auto toRefreshTimeoutMS = gParameters.toRefreshTimeoutMS.load();
    auto pendingTimeoutMS = gParameters.pendingTimeoutMS.load();
    if (hostTimeoutMS >= (toRefreshTimeoutMS + pendingTimeoutMS)) {
        return Status::OK();
    }

    std::string msg = str::stream()
        << "ShardingTaskExecutorPoolHostTimeoutMS (" << hostTimeoutMS
        << ") set below ShardingTaskExecutorPoolRefreshRequirementMS (" << toRefreshTimeoutMS
        << ") + ShardingTaskExecutorPoolRefreshTimeoutMS (" << pendingTimeoutMS << ").";
    return Status(ErrorCodes::BadValue, msg);
}

Status ShardingTaskExecutorPoolController::validatePendingTimeout(const int& pendingTimeoutMS) {
    auto toRefreshTimeoutMS = gParameters.toRefreshTimeoutMS.load();
    if (pendingTimeoutMS < toRefreshTimeoutMS) {
        return Status::OK();
    }

    std::string msg = str::stream()
        << "ShardingTaskExecutorPoolRefreshRequirementMS (" << toRefreshTimeoutMS
        << ") set below ShardingTaskExecutorPoolRefreshTimeoutMS (" << pendingTimeoutMS << ").";
    return Status(ErrorCodes::BadValue, msg);
}

Status ShardingTaskExecutorPoolController::onUpdateMatchingStrategy(const std::string& str) {
    if (str == "automatic") {
        if (isMongos()) {
            gParameters.matchingStrategy.store(MatchingStrategy::kMatchPrimaryNode);
        } else {
            gParameters.matchingStrategy.store(MatchingStrategy::kDisabled);
        }
    } else if (str == "disabled") {
        gParameters.matchingStrategy.store(MatchingStrategy::kDisabled);
    } else if (str == "matchPrimaryNode") {
        gParameters.matchingStrategy.store(MatchingStrategy::kMatchPrimaryNode);
    } else if (str == "matchBusiestNode") {
        gParameters.matchingStrategy.store(MatchingStrategy::kMatchBusiestNode);
    } else {
        return Status{ErrorCodes::BadValue,
                      str::stream() << "Unrecognized matching strategy '" << str << "'"};
    }

    return Status::OK();
}

void ShardingTaskExecutorPoolController::_addGroup(WithLock,
                                                   const ReplicaSetChangeNotifier::State& state) {
    auto groupData = std::make_shared<GroupData>();
    groupData->primary = state.primary;

    // Find each active member
    for (auto& host : state.connStr.getServers()) {
        if (!state.passives.count(host)) {
            groupData->members.push_back(host);
        }
    }

    // Mark each host with this groupData
    for (auto& host : groupData->members) {
        auto& groupAndId = _groupAndIds[host];

        invariant(!groupAndId.groupData);
        groupAndId.groupData = groupData;

        if (groupAndId.maybeId) {
            // There is already a pool registered to this host
            // This group needs to include its id in the list of members and pass its pointer
            auto id = *groupAndId.maybeId;
            getOrInvariant(_poolDatas, id).groupData = groupData;
            emplaceOrInvariant(groupData->poolIds, id);
        }
    }

    emplaceOrInvariant(_groupDatas, state.connStr.getSetName(), std::move(groupData));
}

void ShardingTaskExecutorPoolController::_removeGroup(WithLock, const std::string& name) {
    auto it = _groupDatas.find(name);
    if (it == _groupDatas.end()) {
        return;
    }

    auto& groupData = it->second;
    for (auto& host : groupData->members) {
        auto& groupAndId = getOrInvariant(_groupAndIds, host);
        groupAndId.groupData.reset();
        if (groupAndId.maybeId) {
            // There is still a pool registered to this host, reset its pointer
            getOrInvariant(_poolDatas, *groupAndId.maybeId).groupData.reset();
        } else {
            invariant(_groupAndIds.erase(host));
        }
    }

    _groupDatas.erase(it);
}

class ShardingTaskExecutorPoolController::ReplicaSetChangeListener final
    : public ReplicaSetChangeNotifier::Listener {
public:
    explicit ReplicaSetChangeListener(ShardingTaskExecutorPoolController* controller)
        : _controller(controller) {}

    void onFoundSet(const Key& key) noexcept override {
        // Do nothing
    }

    void onConfirmedSet(const State& state) noexcept override {
        stdx::lock_guard lk(_controller->_mutex);

        _controller->_removeGroup(lk, state.connStr.getSetName());
        _controller->_addGroup(lk, state);
    }

    void onPossibleSet(const State& state) noexcept override {
        // Do nothing
    }

    void onDroppedSet(const Key& key) noexcept override {
        stdx::lock_guard lk(_controller->_mutex);

        _controller->_removeGroup(lk, key);
    }

private:
    ShardingTaskExecutorPoolController* const _controller;
};

void ShardingTaskExecutorPoolController::init(ConnectionPool* parent) {
    ControllerInterface::init(parent);
    _listener = ReplicaSetMonitor::getNotifier().makeListener<ReplicaSetChangeListener>(this);
}

void ShardingTaskExecutorPoolController::addHost(PoolId id, const HostAndPort& host) {
    stdx::lock_guard lk(_mutex);

    PoolData poolData;
    poolData.host = host;
    poolData.isConfigServer = isConfigServer(_shardRegistry.lock().get(), host);

    // Set up the GroupAndId
    auto& groupAndId = _groupAndIds[host];

    invariant(!groupAndId.maybeId);
    groupAndId.maybeId = id;

    if (groupAndId.groupData) {
        // If there is already a GroupData, then get its pointer and the PoolId to its list
        poolData.groupData = groupAndId.groupData;

        emplaceOrInvariant(groupAndId.groupData->poolIds, id);
    }

    // Add this PoolData to the set
    emplaceOrInvariant(_poolDatas, id, std::move(poolData));
}
auto ShardingTaskExecutorPoolController::updateHost(PoolId id, const HostState& stats)
    -> HostGroupState {
    stdx::lock_guard lk(_mutex);

    auto& poolData = getOrInvariant(_poolDatas, id);

    const auto [minConns, maxConns] = [&] {
        size_t lo = gParameters.minConnections.load();
        size_t hi = gParameters.maxConnections.load();
        if (poolData.isConfigServer) {
            auto maybeOverride = [](size_t& t, int val) {
                if (val >= 0)
                    t = val;
            };
            maybeOverride(lo, gParameters.minConnectionsForConfigServers.load());
            maybeOverride(hi, gParameters.maxConnectionsForConfigServers.load());
        }
        return std::tuple(lo, hi);
    }();
    // conn_pool_csrs.js looks for this message in the log.
    LOGV2_DEBUG(6265600,
                5,
                "Update connection pool",
                "host"_attr = poolData.host,
                "minConns"_attr = minConns,
                "maxConns"_attr = maxConns);

    // Update the target for just the pool first
    poolData.target = stats.requests + stats.active;

    if (poolData.target < minConns) {
        poolData.target = minConns;
    } else if (poolData.target > maxConns) {
        poolData.target = maxConns;
    }

    poolData.isAbleToShutdown = stats.health.isExpired;

    // If the pool isn't in a groupData, we can return now
    auto groupData = poolData.groupData.lock();
    if (!groupData) {
        return {{poolData.host}, poolData.isAbleToShutdown};
    }

    switch (gParameters.matchingStrategy.load()) {
        case MatchingStrategy::kMatchPrimaryNode: {
            if (groupData->primary == poolData.host) {
                groupData->target = poolData.target;
            }
        } break;
        case MatchingStrategy::kMatchBusiestNode: {
            groupData->target = 0;
            for (auto otherId : groupData->poolIds) {
                groupData->target =
                    std::max(groupData->target, getOrInvariant(_poolDatas, otherId).target);
            }
        } break;
        case MatchingStrategy::kDisabled: {
            // Nothing
        } break;
    };

    if (groupData->target < minConns) {
        groupData->target = minConns;
    } else if (groupData->target > maxConns) {
        groupData->target = maxConns;
    }

    invariant(!groupData->poolIds.empty());
    auto shouldShutdown = poolData.isAbleToShutdown &&
        std::all_of(groupData->poolIds.begin(), groupData->poolIds.end(), [&](auto otherId) {
                              return getOrInvariant(_poolDatas, otherId).isAbleToShutdown;
                          });
    return {groupData->members, shouldShutdown};
}

void ShardingTaskExecutorPoolController::removeHost(PoolId id) {
    stdx::lock_guard lk(_mutex);
    auto it = _poolDatas.find(id);
    if (it == _poolDatas.end()) {
        // It's possible that a host immediately needs to go before it updates even once
        return;
    }

    auto& poolData = it->second;
    auto& groupAndId = getOrInvariant(_groupAndIds, poolData.host);
    groupAndId.maybeId.reset();
    if (groupAndId.groupData) {
        invariant(groupAndId.groupData->poolIds.erase(id));
    } else {
        invariant(_groupAndIds.erase(poolData.host));
    }

    _poolDatas.erase(it);
}

auto ShardingTaskExecutorPoolController::getControls(PoolId id) -> ConnectionControls {
    stdx::lock_guard lk(_mutex);
    auto& poolData = getOrInvariant(_poolDatas, id);

    const size_t maxPending = gParameters.maxConnecting.load();

    auto groupData = poolData.groupData.lock();
    if (!groupData || gParameters.matchingStrategy.load() == MatchingStrategy::kDisabled) {
        return {maxPending, poolData.target};
    }

    auto target = std::max(poolData.target, groupData->target);
    return {maxPending, target};
}

Milliseconds ShardingTaskExecutorPoolController::hostTimeout() const {
    return Milliseconds{gParameters.hostTimeoutMS.load()};
}

Milliseconds ShardingTaskExecutorPoolController::pendingTimeout() const {
    return Milliseconds{gParameters.pendingTimeoutMS.load()};
}

Milliseconds ShardingTaskExecutorPoolController::toRefreshTimeout() const {
    return Milliseconds{gParameters.toRefreshTimeoutMS.load()};
}

void ShardingTaskExecutorPoolController::updateConnectionPoolStats(
    executor::ConnectionPoolStats* cps) const {
    cps->strategy = gParameters.matchingStrategy.load();
}

}  // namespace mongo
