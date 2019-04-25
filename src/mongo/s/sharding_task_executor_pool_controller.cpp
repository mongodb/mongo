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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kConnectionPool

#include "mongo/platform/basic.h"

#include "mongo/client/replica_set_monitor.h"
#include "mongo/s/sharding_task_executor_pool_controller.h"
#include "mongo/util/log.h"

namespace mongo {

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
    // TODO Fix up after SERVER-40224
    if (str == "disabled") {
        gParameters.matchingStrategy.store(MatchingStrategy::kDisabled);
    } else if (str == "matchPrimaryNode") {
        gParameters.matchingStrategy.store(MatchingStrategy::kMatchPrimaryNode);
        // TODO Reactive once the prediction pattern is fixed in SERVER-41602
        //} else if (str == "matchBusiestNode") {
        //    gParameters.matchingStrategy.store(MatchingStrategy::kMatchBusiestNode);
    } else {
        return Status{ErrorCodes::BadValue,
                      str::stream() << "Unrecognized matching strategy '" << str << "'"};
    }

    return Status::OK();
}

void ShardingTaskExecutorPoolController::_addGroup(WithLock,
                                                   const ReplicaSetChangeNotifier::State& state) {
    // Replace the last group
    auto& group = _hostGroups[state.connStr.getSetName()];
    group = std::make_shared<HostGroupData>();
    group->state = state;

    // Mark each host with this group
    for (auto& host : state.connStr.getServers()) {
        _hostGroupsByHost[host] = group;
    }
}

void ShardingTaskExecutorPoolController::_removeGroup(WithLock, const std::string& name) {
    auto it = _hostGroups.find(name);
    if (it == _hostGroups.end()) {
        return;
    }

    auto& hostGroup = it->second;
    for (auto& host : hostGroup->state.connStr.getServers()) {
        _hostGroupsByHost.erase(host);
    }
    _hostGroups.erase(it);
}

class ShardingTaskExecutorPoolController::ReplicaSetChangeListener final
    : public ReplicaSetChangeNotifier::Listener {
public:
    explicit ReplicaSetChangeListener(ShardingTaskExecutorPoolController* controller)
        : _controller(controller) {}

    void onFoundSet(const Key& key) override {
        // Do nothing
    }

    void onConfirmedSet(const State& state) override {
        stdx::lock_guard lk(_controller->_mutex);

        _controller->_removeGroup(lk, state.connStr.getSetName());
        _controller->_addGroup(lk, state);
    }

    void onPossibleSet(const State& state) override {
        // Do nothing
    }

    void onDroppedSet(const Key& key) override {
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

auto ShardingTaskExecutorPoolController::updateHost(const SpecificPool* pool,
                                                    const HostAndPort& host,
                                                    const HostState& stats) -> HostGroup {
    stdx::lock_guard lk(_mutex);

    auto& data = _poolData[pool];

    const size_t minConns = gParameters.minConnections.load();
    const size_t maxConns = gParameters.maxConnections.load();

    // Update the target for just the pool first
    data.target = stats.requests + stats.active;

    if (data.target < minConns) {
        data.target = minConns;
    } else if (data.target > maxConns) {
        data.target = maxConns;
    }

    data.isAbleToShutdown = stats.health.isExpired;

    // If the pool isn't in a group, we can return now
    auto it = _hostGroupsByHost.find(host);
    if (it == _hostGroupsByHost.end()) {
        return {{host}, data.isAbleToShutdown};
    }

    // If the pool has a group, then update the group
    auto& hostGroup = it->second;
    data.hostGroup = hostGroup;

    // Make sure we're part of the group
    hostGroup->pools.insert(pool);

    switch (gParameters.matchingStrategy.load()) {
        case MatchingStrategy::kMatchPrimaryNode: {
            if (hostGroup->state.primary == host) {
                hostGroup->target = data.target;
            }
        } break;
        case MatchingStrategy::kMatchBusiestNode: {
            hostGroup->target = std::max(hostGroup->target, data.target);
        } break;
        case MatchingStrategy::kDisabled: {
            // Nothing
        } break;
    };

    if (hostGroup->target < minConns) {
        hostGroup->target = minConns;
    } else if (hostGroup->target > maxConns) {
        hostGroup->target = maxConns;
    }

    auto shouldShutdown = data.isAbleToShutdown &&
        std::all_of(hostGroup->pools.begin(), hostGroup->pools.end(), [&](auto otherPool) {
                              return _poolData[otherPool].isAbleToShutdown;
                          });
    return {hostGroup->state.connStr.getServers(), shouldShutdown};
}

void ShardingTaskExecutorPoolController::removeHost(const SpecificPool* pool) {
    stdx::lock_guard lk(_mutex);
    auto it = _poolData.find(pool);
    if (it == _poolData.end()) {
        // It's possible that a host immediately needs to go before it updates even once
        return;
    }

    auto& data = it->second;
    if (auto hostGroup = data.hostGroup.lock()) {
        hostGroup->pools.erase(pool);
    }
    _poolData.erase(it);
}

auto ShardingTaskExecutorPoolController::getControls(const SpecificPool* pool)
    -> ConnectionControls {
    stdx::lock_guard lk(_mutex);
    auto& data = _poolData[pool];

    const size_t maxPending = gParameters.maxConnecting.load();

    auto hostGroup = data.hostGroup.lock();
    if (!hostGroup || gParameters.matchingStrategy.load() == MatchingStrategy::kDisabled) {
        return {maxPending, data.target};
    }

    auto target = std::max(data.target, hostGroup->target);
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

}  // namespace mongo
