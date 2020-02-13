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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/db/repl/topology_version_observer.h"

#include "mongo/base/status_with.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

void TopologyVersionObserver::init(ReplicationCoordinator* replCoordinator) noexcept {
    invariant(replCoordinator);
    stdx::lock_guard<Mutex> lk(_mutex);
    invariant(_state.load() == State::kUninitialized);

    LOGV2_INFO(40440,
               "Starting {topologyVersionObserverName}",
               "topologyVersionObserverName"_attr = toString());
    _replCoordinator = replCoordinator;

    invariant(!_thread);
    _thread = stdx::thread([&]() { this->_workerThreadBody(); });

    // Wait for the observer thread to update the status.
    while (_state.load() != State::kRunning) {
    }
}

void TopologyVersionObserver::shutdown() noexcept {
    stdx::unique_lock<Mutex> lk(_mutex);
    if (_state.load() == State::kUninitialized) {
        return;
    }

    // Check if another `shutdown()` has already completed.
    if (!_thread) {
        return;
    }

    LOGV2_INFO(40441,
               "Stopping {topologyVersionObserverName}",
               "topologyVersionObserverName"_attr = toString());
    auto state = _state.load();
    invariant(state == State::kRunning || state == State::kShutdown);

    // Wait for the observer client to exit from its main loop.
    // Observer thread must update the state before attempting to acquire the mutex.
    while (_state.load() == State::kRunning) {
        invariant(_observerClient);
        _observerClient->lock();
        auto opCtx = _observerClient->getOperationContext();
        if (opCtx) {
            opCtx->markKilled(ErrorCodes::ShutdownInProgress);
        }
        _observerClient->unlock();
    }

    invariant(_state.load() == State::kShutdown);
    auto thread = std::exchange(_thread, boost::none);
    lk.unlock();

    invariant(thread);
    thread->join();
}

std::shared_ptr<const IsMasterResponse> TopologyVersionObserver::getCached() noexcept {
    // Acquires the lock to avoid potential races with `_workerThreadBody()`.
    // Atomics cannot be used here as `shared_ptr` cannot be atomically updated.
    stdx::lock_guard<Mutex> lk(_mutex);
    return _cache;
}

std::string TopologyVersionObserver::toString() const {
    return str::stream() << kTopologyVersionObserverName;
}

std::shared_ptr<const IsMasterResponse> TopologyVersionObserver::_getIsMasterResponse(
    boost::optional<TopologyVersion> topologyVersion, bool* shouldShutdown) try {
    invariant(*shouldShutdown == false);
    ServiceContext::UniqueOperationContext opCtx;
    try {
        opCtx = Client::getCurrent()->makeOperationContext();
    } catch (...) {
        // Failure to create an operation context could cause deadlocks.
        *shouldShutdown = true;
        LOGV2_WARNING(40442, "Observer was unable to create a new OperationContext.");
        return nullptr;
    }

    invariant(opCtx);
    auto future = _replCoordinator->getIsMasterResponseFuture({}, topologyVersion);
    auto response = future.get(opCtx.get());
    if (!response->isConfigSet()) {
        return nullptr;
    }

    return response;
} catch (const ExceptionForCat<ErrorCategory::ShutdownError>& e) {
    LOGV2_WARNING(
        40443, "Observer was interrupted by {exception}", "exception"_attr = e.toString());
    *shouldShutdown = true;
    return nullptr;
} catch (DBException& e) {
    LOGV2_WARNING(40444,
                  "Observer could not retrieve isMasterResponse: {exception}",
                  "exception"_attr = e.toString());
    return nullptr;
}

void TopologyVersionObserver::_workerThreadBody() noexcept {
    invariant(_state.load() == State::kUninitialized);

    // Creates a new client and makes `_observerClient` to point to it, which allows `shutdown()`
    // to access the client object.
    Client::initThread(kTopologyVersionObserverName);
    _observerClient = Client::getCurrent();

    // `init()` must hold the mutex until the observer updates the state.
    invariant(!_mutex.try_lock());
    // The following notifies `init()` that `_observerClient` is set and ready to use.
    _state.store(State::kRunning);

    auto getTopologyVersion = [&]() -> boost::optional<TopologyVersion> {
        // Only the observer thread updates `_cache`, thus there is no need to hold the lock before
        // accessing `_cache` here.
        if (_cache) {
            return _cache->getTopologyVersion();
        }
        return boost::none;
    };

    ON_BLOCK_EXIT([&] {
        // Once the observer detects a shutdown, it must update the state first before attempting
        // to acquire the lock. This is necessary to avoid deadlocks.
        invariant(_state.load() == State::kRunning);
        _state.store(State::kShutdown);

        stdx::unique_lock lock(_mutex);

        // Invalidate the cache as it is no longer updated
        _cache.reset();

        // Client object is local to this thread, and is no longer be available.
        _observerClient = nullptr;

        LOGV2_INFO(40447,
                   "Stopped {topologyVersionObserverName}",
                   "topologyVersionObserverName"_attr = toString());
    });

    bool shouldShutdown = false;
    LOGV2_INFO(40445,
               "Started {topologyVersionObserverName}",
               "topologyVersionObserverName"_attr = toString());
    while (!shouldShutdown) {
        auto response = _getIsMasterResponse(getTopologyVersion(), &shouldShutdown);
        // Only update if the version is more recent than the cached version, or `_cache` is null.
        if (!shouldShutdown && response != _cache) {
            stdx::lock_guard lock(_mutex);
            _cache = response;
        }
    }
}

}  // namespace repl
}  // namespace mongo
