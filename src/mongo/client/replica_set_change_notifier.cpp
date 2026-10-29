// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/client/replica_set_change_notifier.h"

#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <mutex>

#include <absl/container/node_hash_map.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

void ReplicaSetChangeNotifier::_addListener(std::shared_ptr<Listener> listener) {
    std::lock_guard lk(_mutex);

    listener->init(this);
    _listeners.push_back(std::move(listener));
}

void ReplicaSetChangeNotifier::onFoundSet(const std::string& name) {
    LOGV2_DEBUG(20158, 2, "Signaling found set", "replicaSet"_attr = name);

    std::unique_lock<std::mutex> lk(_mutex);

    _replicaSetStates.emplace(name, State{});

    auto listeners = _listeners;
    lk.unlock();

    for (const auto& listener : listeners) {
        if (auto l = listener.lock()) {
            l->onFoundSet(name);
        }
    };
}

void ReplicaSetChangeNotifier::onPossibleSet(ConnectionString connectionString) {
    LOGV2_DEBUG(20159, 2, "Signaling possible set", "connectionString"_attr = connectionString);

    const auto& name = connectionString.getSetName();

    std::unique_lock<std::mutex> lk(_mutex);

    auto state = [&] {
        auto& state = _replicaSetStates[name];
        ++state.generation;

        state.connStr = std::move(connectionString);
        state.primary = {};

        return state;
    }();

    auto listeners = _listeners;
    lk.unlock();

    for (const auto& listener : listeners) {
        if (auto l = listener.lock()) {
            l->onPossibleSet(state);
        }
    };
}

void ReplicaSetChangeNotifier::onConfirmedSet(ConnectionString connectionString,
                                              HostAndPort primary,
                                              std::set<HostAndPort> passives) {
    LOGV2_DEBUG(20160,
                2,
                "Signaling confirmed set with primary",
                "connectionString"_attr = connectionString,
                "primary"_attr = primary);

    const auto& name = connectionString.getSetName();
    std::unique_lock<std::mutex> lk(_mutex);

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

    for (const auto& listener : listeners) {
        if (auto l = listener.lock()) {
            l->onConfirmedSet(state);
        }
    };
}

void ReplicaSetChangeNotifier::onDroppedSet(const std::string& name) {
    LOGV2_DEBUG(20161, 2, "Signaling dropped set", "replicaSet"_attr = name);

    std::unique_lock<std::mutex> lk(_mutex);

    // If we never singaled the initial possible set, we should not on dropped set
    auto it = _replicaSetStates.find(name);
    if (it == _replicaSetStates.end()) {
        return;
    }

    _replicaSetStates.erase(it);

    auto listeners = _listeners;
    lk.unlock();

    for (const auto& listener : listeners) {
        if (auto l = listener.lock()) {
            l->onDroppedSet(name);
        }
    };
}

auto ReplicaSetChangeNotifier::Listener::getCurrentState(const Key& key) -> State {
    invariant(_notifier);

    std::lock_guard lk(_notifier->_mutex);

    return _notifier->_replicaSetStates.at(key);
}

}  // namespace mongo
