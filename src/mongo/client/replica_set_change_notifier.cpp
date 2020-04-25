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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/replica_set_change_notifier.h"

#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

void ReplicaSetChangeNotifier::_addListener(std::shared_ptr<Listener> listener) {
    stdx::lock_guard lk(_mutex);

    listener->init(this);
    _listeners.push_back(listener);
}

void ReplicaSetChangeNotifier::onFoundSet(const std::string& name) noexcept {
    LOGV2_DEBUG(20158, 2, "Signaling found set {name}", "name"_attr = name);

    stdx::unique_lock<Latch> lk(_mutex);

    _replicaSetStates.emplace(name, State{});

    auto listeners = _listeners;
    lk.unlock();

    for (auto listener : listeners) {
        if (auto l = listener.lock()) {
            l->onFoundSet(name);
        }
    };
}

void ReplicaSetChangeNotifier::onPossibleSet(ConnectionString connectionString) noexcept {
    LOGV2_DEBUG(20159,
                2,
                "Signaling possible set {connectionString}",
                "connectionString"_attr = connectionString);

    const auto& name = connectionString.getSetName();

    stdx::unique_lock<Latch> lk(_mutex);

    auto state = [&] {
        auto& state = _replicaSetStates[name];
        ++state.generation;

        state.connStr = std::move(connectionString);
        state.primary = {};

        return state;
    }();

    auto listeners = _listeners;
    lk.unlock();

    for (auto listener : listeners) {
        if (auto l = listener.lock()) {
            l->onPossibleSet(state);
        }
    };
}

void ReplicaSetChangeNotifier::onConfirmedSet(ConnectionString connectionString,
                                              HostAndPort primary,
                                              std::set<HostAndPort> passives) noexcept {
    LOGV2_DEBUG(20160,
                2,
                "Signaling confirmed set {connectionString} with primary {primary}",
                "connectionString"_attr = connectionString,
                "primary"_attr = primary);

    const auto& name = connectionString.getSetName();
    stdx::unique_lock<Latch> lk(_mutex);

    auto state = [&] {
        auto& state = _replicaSetStates[name];
        ++state.generation;

        state.connStr = std::move(connectionString);
        state.primary = std::move(primary);
        state.passives = std::move(passives);

        return state;
    }();

    auto listeners = _listeners;
    lk.unlock();

    for (auto listener : listeners) {
        if (auto l = listener.lock()) {
            l->onConfirmedSet(state);
        }
    };
}

void ReplicaSetChangeNotifier::onDroppedSet(const std::string& name) noexcept {
    LOGV2_DEBUG(20161, 2, "Signaling dropped set {name}", "name"_attr = name);

    stdx::unique_lock<Latch> lk(_mutex);

    // If we never singaled the initial possible set, we should not on dropped set
    auto it = _replicaSetStates.find(name);
    if (it == _replicaSetStates.end()) {
        return;
    }

    _replicaSetStates.erase(it);

    auto listeners = _listeners;
    lk.unlock();

    for (auto listener : listeners) {
        if (auto l = listener.lock()) {
            l->onDroppedSet(name);
        }
    };
}

auto ReplicaSetChangeNotifier::Listener::getCurrentState(const Key& key) -> State {
    invariant(_notifier);

    stdx::lock_guard lk(_notifier->_mutex);

    return _notifier->_replicaSetStates.at(key);
}

}  // namespace mongo
