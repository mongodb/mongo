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


#include "mongo/db/repl/topology_version_observer.h"

#include "mongo/base/status_with.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(topologyVersionObserverExpectsInterruption);
MONGO_FAIL_POINT_DEFINE(topologyVersionObserverExpectsShutdown);
MONGO_FAIL_POINT_DEFINE(topologyVersionObserverBeforeCheckingForShutdown);
MONGO_FAIL_POINT_DEFINE(topologyVersionObserverShutdownShouldWait);

void TopologyVersionObserver::init(ServiceContext* serviceContext,
                                   ReplicationCoordinator* replCoordinator) noexcept {
    LOGV2_INFO(40440, "Starting the TopologyVersionObserver");

    stdx::unique_lock lk(_mutex);

    _serviceContext = serviceContext;
    invariant(_serviceContext);

    _replCoordinator =
        replCoordinator ? replCoordinator : ReplicationCoordinator::get(_serviceContext);
    invariant(_replCoordinator);

    invariant(!_thread);
    invariant(_state.load() == State::kUninitialized);
    _thread = stdx::thread([&]() { this->_workerThreadBody(); });

    _cv.wait(lk, [&] { return _state.load() != State::kUninitialized; });
}

void TopologyVersionObserver::shutdown() noexcept {
    auto shouldWaitForShutdown = _shouldShutdown.swap(true);
    if (shouldWaitForShutdown) {
        // If we aren't the first ones to call shutdown, wait for the thread to stop
        stdx::unique_lock lk(_mutex);

        _cv.wait(lk, [&] { return _state.load() == State::kShutdown; });
        invariant(_state.load() == State::kShutdown);
        return;
    }

    LOGV2_INFO(40441, "Stopping TopologyVersionObserver");

    // Wait for the thread to stop and steal it to the local stack
    auto thread = [&] {
        stdx::unique_lock lk(_mutex);

        // If we are still running, attempt to kill any opCtx
        if (_workerOpCtx) {
            stdx::lock_guard clientLk(*_workerOpCtx->getClient());
            _serviceContext->killOperation(clientLk, _workerOpCtx, ErrorCodes::ShutdownInProgress);
        }

        topologyVersionObserverShutdownShouldWait.pauseWhileSet();
        _cv.wait(lk, [&] { return _state.load() != State::kRunning; });

        invariant(_state.load() == State::kShutdown);

        return std::exchange(_thread, boost::none);
    }();

    if (!thread) {
        // We never started
        return;
    }

    // Finally join
    thread->join();
}

std::shared_ptr<const HelloResponse> TopologyVersionObserver::getCached() noexcept {
    if (_state.load() != State::kRunning || _shouldShutdown.load()) {
        // Early return if we know there isn't a worker
        return {};
    }

    // Acquires the lock to avoid potential races with `_workerThreadBody()`.
    // Atomics cannot be used here as `shared_ptr` cannot be atomically updated.
    stdx::lock_guard<Mutex> lk(_mutex);
    return _cache;
}

std::string TopologyVersionObserver::toString() const {
    return str::stream() << kTopologyVersionObserverName;
}

void TopologyVersionObserver::_cacheHelloResponse(
    OperationContext* opCtx, boost::optional<TopologyVersion> topologyVersion) try {
    invariant(opCtx);

    LOGV2_DEBUG(4794600, 3, "Waiting for a topology change");

    {
        ScopeGuard cacheGuard([&] {
            // If we're not dismissed, reset the _cache.
            stdx::lock_guard lk(_mutex);
            _cache.reset();
        });

        invariant(_replCoordinator);
        auto future = _replCoordinator->getHelloResponseFuture({}, topologyVersion);

        if (auto response = std::move(future).get(opCtx); response->isConfigSet()) {
            stdx::lock_guard lk(_mutex);
            _cache = response;

            // Reset the cacheGuard because we got a good value.
            cacheGuard.dismiss();
        }
    }

    if (_shouldShutdown.load()) {
        // Pessimistically check if we should shutdown before we sleepFor(...).
        return;
    }

    LOGV2_DEBUG(4794601, 3, "Observed a topology change");

    // We could be a PeriodicRunner::Job someday. For now, OperationContext::sleepFor() will serve
    // the same purpose.
    opCtx->sleepFor(kDelayMS);
} catch (const ExceptionFor<ErrorCodes::InterruptedDueToStorageChange>& e) {
    LOGV2_DEBUG(5929701,
                1,
                "Caught an InterruptedDueToStorageChange exception, "
                "but this thread can safely continue",
                "error"_attr = e.toStatus());
} catch (const DBException& e) {
    if (ErrorCodes::isShutdownError(e)) {
        // Rethrow if we've experienced shutdown.
        throw;
    }

    LOGV2_WARNING(40444, "Observer could not retrieve HelloResponse", "error"_attr = e.toString());
}

void TopologyVersionObserver::_workerThreadBody() noexcept try {
    invariant(_serviceContext);
    ThreadClient tc(kTopologyVersionObserverName, _serviceContext);

    // TODO(SERVER-74656): Please revisit if this thread could be made killable.
    {
        stdx::lock_guard<Client> lk(*tc.get());
        tc.get()->setSystemOperationUnkillableByStepdown(lk);
    }

    auto getTopologyVersion = [&]() -> boost::optional<TopologyVersion> {
        // Only the observer thread updates `_cache`, thus there is no need to hold the lock before
        // accessing `_cache` here.
        if (_cache) {
            return _cache->getTopologyVersion();
        }
        return boost::none;
    };

    LOGV2_INFO(40445, "Started TopologyVersionObserver");

    {
        stdx::lock_guard lk(_mutex);
        invariant(_state.load() == State::kUninitialized);
        if (_shouldShutdown.load()) {
            _state.store(State::kShutdown);
            _cv.notify_all();

            return;
        }

        // The following notifies `init()` that the worker thread is active.
        _state.store(State::kRunning);
        _cv.notify_all();
    }

    ON_BLOCK_EXIT([&] {
        {
            stdx::lock_guard lk(_mutex);
            invariant(_state.load() == State::kRunning);
            invariant(_workerOpCtx == nullptr);
            _state.store(State::kShutdown);

            // Invalidate the cache as it is no longer updated
            _cache.reset();

            // Notify `shutdown()` that the worker thread is no longer active
            _cv.notify_all();
        }

        LOGV2_INFO(40447, "Stopped TopologyVersionObserver");

        // Pause here to confirm that we do not depend upon shutdown() being invoked for
        // isShutdown() to be true.
        topologyVersionObserverExpectsShutdown.pauseWhileSet();
    });

    while (true) {
        auto opCtxHandle = tc->makeOperationContext();
        topologyVersionObserverBeforeCheckingForShutdown.pauseWhileSet();

        {
            // Set the _workerOpCtx to our newly formed opCtxHandle before we unlock.
            stdx::lock_guard lk(_mutex);
            // Checking `_shouldShutdown` under the lock is necessary to ensure the shutdown
            // method can interrupt the new operation.
            if (_shouldShutdown.load())
                break;
            _workerOpCtx = opCtxHandle.get();
        }

        ON_BLOCK_EXIT([&] {
            // We're done with our opCtxHandle, unset _workerOpCtx.
            stdx::lock_guard lk(_mutex);
            _workerOpCtx = nullptr;
        });

        // Pause here so that we can force there to be an opCtx to be interrupted.
        topologyVersionObserverExpectsInterruption.pauseWhileSet();

        _cacheHelloResponse(opCtxHandle.get(), getTopologyVersion());
    }
} catch (const ExceptionForCat<ErrorCategory::ShutdownError>& e) {
    LOGV2_DEBUG(40443, 3, "Observer thread stopped due to shutdown", "error"_attr = e.toString());
}

}  // namespace repl
}  // namespace mongo
