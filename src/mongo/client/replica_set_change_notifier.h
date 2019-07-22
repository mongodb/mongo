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

#pragma once

#include <string>
#include <type_traits>
#include <vector>

#include "mongo/client/connection_string.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/functional.h"

namespace mongo {

/**
 * A stateful notifier for events from a set of ReplicaSetMonitors
 */
class ReplicaSetChangeNotifier {
public:
    using Key = std::string;
    class Listener;
    using ListenerHandle = std::unique_ptr<Listener, unique_function<void(Listener*)>>;
    struct State;

public:
    ReplicaSetChangeNotifier() = default;
    ReplicaSetChangeNotifier(const ReplicaSetChangeNotifier&) = delete;
    ReplicaSetChangeNotifier(ReplicaSetChangeNotifier&&) = delete;
    ReplicaSetChangeNotifier& operator=(const ReplicaSetChangeNotifier&) = delete;
    ReplicaSetChangeNotifier& operator=(ReplicaSetChangeNotifier&&) = delete;

    /**
     *  Notify every listener that there is a new ReplicaSet and initialize the State
     */
    void onFoundSet(const std::string& replicaSet);

    /**
     * Notify every listener that a scan completed without finding a primary and update
     */
    void onPossibleSet(ConnectionString connectionString);

    /**
     * Notify every listener that a scan completed and found a new primary or config
     */
    void onConfirmedSet(ConnectionString connectionString,
                        HostAndPort primary,
                        std::set<HostAndPort> passives);

    /**
     * Notify every listener that a ReplicaSet is no longer in use and drop the State
     */
    void onDroppedSet(const std::string& replicaSet);

    /**
     * Create a listener of a given type and bind it to this notifier
     */
    template <typename DerivedT,
              typename... Args,
              typename = std::enable_if_t<std::is_constructible_v<DerivedT, Args...>>>
    auto makeListener(Args&&... args) {
        auto deleter = [this](auto listener) {
            _removeListener(listener);
            delete listener;
        };
        auto ptr = new DerivedT(std::forward<Args>(args)...);

        _addListener(ptr);

        return ListenerHandle(ptr, std::move(deleter));
    }

private:
    void _addListener(Listener* listener);
    void _removeListener(Listener* listener);

    stdx::mutex _mutex;
    std::vector<Listener*> _listeners;
    stdx::unordered_map<Key, State> _replicaSetStates;
};

/**
 * A listener for events from a set of ReplicaSetMonitors
 *
 * This class will normally be notified of events for every replica set in use in the system.
 * The onSet functions are all called syncronously by the notifier,
 * if your implementation would block or seriously delay execution,
 * please schedule the majority of the work to complete asynchronously.
 */
class ReplicaSetChangeNotifier::Listener {
public:
    using Notifier = ReplicaSetChangeNotifier;
    using Key = typename Notifier::Key;
    using State = typename Notifier::State;

public:
    Listener(const Listener&) = delete;
    Listener(Listener&&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener& operator=(Listener&&) = delete;

    Listener() = default;
    virtual ~Listener() = default;

    /**
     * Initialize this listener with a notifier
     */
    void init(Notifier* notifier) {
        _notifier = notifier;
    }

    /**
     * React to a new ReplicaSet that will soon be scanned
     */
    virtual void onFoundSet(const Key& key) = 0;

    /**
     * React to a finished scan that found no primary
     */
    virtual void onPossibleSet(const State& data) = 0;

    /**
     * React to a finished scan that found a primary
     */
    virtual void onConfirmedSet(const State& data) = 0;

    /**
     * React to a ReplicaSet being dropped from use
     */
    virtual void onDroppedSet(const Key& key) = 0;

    /**
     * Get the State as of the last signal function invoked on the Notifier
     */
    State getCurrentState(const Key& key);

private:
    Notifier* _notifier = nullptr;
};

using ReplicaSetChangeListenerHandle = ReplicaSetChangeNotifier::ListenerHandle;

struct ReplicaSetChangeNotifier::State {
    ConnectionString connStr;
    HostAndPort primary;
    std::set<HostAndPort> passives;

    int64_t generation = 0;
};

}  // namespace mongo
