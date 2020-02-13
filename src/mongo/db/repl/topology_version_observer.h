/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <memory>
#include <string>

#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/time_support.h"

namespace mongo {

class Client;

namespace repl {

constexpr auto kTopologyVersionObserverName = "TopologyVersionObserver";

/**
 * An utility to observe topology changes asynchronously and cache updates.
 * `getCached()` is thread-safe (through `_mutex`), but its behavior is undefined during the
 * execution of the constructor/destructor methods.
 *
 * The life-cycle for instances of this class (`_state`) is as follows:
 * * Uninitialized: immediately after construction.
 *   Call `init()` to initialize the instance and start the observer thread.
 *   You may only call `init()` once -- otherwise, it will terminate the process.
 * * Running: anytime after returning from `init()` and before calling `shutdown()`.
 *   Note that if the observer thread stops due to an error, it will set the state to Shutdown.
 * * Shutdown: the object is ready for destruction.
 *   You must wait for `shutdown()` to return before deleting the object.
 *   Multiple, multithreaded calls to `shutdown()` is safe, and only one will proceed.
 *   After transitioning to shutdown, you can only call the destructor.
 *
 * constructor() -> init() -> getCached() ... getCached() -> shutdown() -> destructor()
 */
class TopologyVersionObserver final {
public:
    TopologyVersionObserver() {}

    TopologyVersionObserver(const TopologyVersionObserver&) = delete;

    TopologyVersionObserver(TopologyVersionObserver&&) noexcept = delete;

    TopologyVersionObserver& operator=(const TopologyVersionObserver&) = delete;

    TopologyVersionObserver& operator=(TopologyVersionObserver&&) = delete;

    ~TopologyVersionObserver() {
        auto state = _state.load();
        invariant(state == State::kShutdown || state == State::kUninitialized);
    }

    void init(ReplicationCoordinator* replCoordinator) noexcept;

    void shutdown() noexcept;

    /**
     * Returns a reference (shared pointer) to the cached version of `IsMasterResponse`.
     * Note that the reference is initially set to `nullptr`.
     * Also, the reference is set back to `nullptr` if the thread that updates `_cache` terminates
     * due to an error (i.e., exception), or it receives an invalid response.
     */
    std::shared_ptr<const IsMasterResponse> getCached() noexcept;

    std::string toString() const;

private:
    enum class State {
        kUninitialized,
        kRunning,
        kShutdown,
    };

    std::shared_ptr<const IsMasterResponse> _getIsMasterResponse(boost::optional<TopologyVersion>,
                                                                 bool*);

    void _workerThreadBody() noexcept;

    /**
     * Protects shared accesses to `_cache`, `_observerClient`, and serializes calls to `init()`
     * and `shutdown()` methods.
     *
     * Accessing the cached `IsMasterResponse` follows a single-producer, multi-consumer model:
     * consumers are readers of `_cache` and the producer is the observer thread. The assumption
     * is that the contention on this lock is insignificant.
     */
    mutable Mutex _mutex = MONGO_MAKE_LATCH(kTopologyVersionObserverName);

    // The reference to the latest cached version of `IsMasterResponse`
    std::shared_ptr<const IsMasterResponse> _cache;

    // Holds a reference to the observer client to allow `shutdown()` to stop the observer thread.
    Client* _observerClient;

    /**
     * Represents the current state of the observer.
     * Note that the observer thread never updates the state.
     */
    AtomicWord<State> _state;

    boost::optional<stdx::thread> _thread;

    ReplicationCoordinator* _replCoordinator;
};

}  // namespace repl
}  // namespace mongo
