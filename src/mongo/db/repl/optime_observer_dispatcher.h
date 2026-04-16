/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/repl/optime_observer.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/waitable_atomic.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/with_lock.h"

#include <memory>
#include <vector>

namespace mongo::repl {

/**
 * Dispatches `onOpTime` notifications to registered observers on a dedicated background thread.
 * The thread is started lazily on the first `addObserver` call and is stopped by `shutdown()`.
 *
 * Coalescing: this dispatcher may skip intermediate opTime values when multiple `notify()` calls
 * arrive faster than the background thread can drain them. This is intentional. Guaranteeing
 * delivery of every opTime would require `notify()` to block until the background thread
 * acknowledges each call, introducing back-pressure onto callers that may hold critical locks.
 * Observers are guaranteed to observe the most recently applied opTime eventually, but not
 * necessarily every intermediate value.
 *
 * Thread-safety:
 * - `notify()` is entirely lock-free and safe to call while holding any external lock.
 * - `addObserver()` may be called at any time, including after the thread has started.
 *
 * Constraints on observer implementations:
 * - `onOpTime()` must not call `shutdown()` on this dispatcher. Doing so would cause the
 *   dispatcher thread to join itself, which is undefined behaviour.
 */
class OpTimeObserverDispatcher {
public:
    using Observer = OpTimeObserver;

    OpTimeObserverDispatcher() = default;

    ~OpTimeObserverDispatcher() {
        shutdown();
    }

    OpTimeObserverDispatcher(const OpTimeObserverDispatcher&) = delete;
    OpTimeObserverDispatcher& operator=(const OpTimeObserverDispatcher&) = delete;

    /**
     * Registers an observer, transferring ownership to the dispatcher. Starts the dispatcher
     * thread lazily on the first call. Observers are never removed.
     */
    void addObserver(std::unique_ptr<Observer> observer) {
        stdx::lock_guard lk(_mutex);
        _observers.push_back(std::move(observer));
        if (_observers.size() == 1) {
            _thread = stdx::thread(&OpTimeObserverDispatcher::_run, this);
        }
    }

    /**
     * Signals the dispatcher that the opTime has advanced. This is cheap and lock-free. Safe to
     * call while holding any external lock (e.g. the ReplicationCoordinator mutex).
     */
    void notify(WithLock, const Timestamp& ts) {
        _pendingTs.storeRelaxed(ts.asULL());
        _event.fetchAndAdd(1);
        _event.notifyAll();
    }

    /**
     * Shuts down the dispatcher thread. Must be called before destruction when at least one
     * observer has been registered. No-op if no observers were ever added.
     */
    void shutdown() {
        _shutdown.storeRelaxed(true);
        _event.fetchAndAdd(1);
        _event.notifyAll();

        auto thread = [&] {
            stdx::lock_guard lk(_mutex);
            return std::exchange(_thread, {});
        }();
        if (thread.joinable()) {
            thread.join();
        }
    }

private:
    void _run() {
        uint64_t lastEvent = 0;

        // Flat snapshot of the observer list. Because observers are only ever appended,
        // we extend this vector incrementally rather than rebuilding it from scratch.
        std::vector<Observer*> observers;

        while (true) {
            // Block until _event differs from lastEvent and return the new value.
            lastEvent = _event.wait(lastEvent);

            if (_shutdown.loadRelaxed()) {
                return;
            }

            // Append any observers added since the last wakeup.
            {
                stdx::lock_guard lk(_mutex);
                for (size_t i = observers.size(); i < _observers.size(); ++i) {
                    observers.push_back(_observers[i].get());
                }
            }

            const Timestamp ts(_pendingTs.loadRelaxed());
            for (auto* obs : observers) {
                obs->onOpTime(ts);
            }
        }
    }

    Atomic<unsigned long long> _pendingTs{0};
    Atomic<bool> _shutdown{false};

    // Event counter. Incremented by notify() and shutdown(). _run() blocks on it waiting for
    // changes.
    WaitableAtomic<uint64_t> _event{0};

    stdx::mutex _mutex;
    // Observers are only ever appended, so _observers.size() serves as a monotonic
    // generation number: _run() re-snapshots only when the size has grown.
    std::vector<std::unique_ptr<Observer>> _observers;  // guarded by _mutex

    stdx::thread _thread;
};

}  // namespace mongo::repl
