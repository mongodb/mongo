// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/hello/topology_version_observer.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <mutex>
#include <type_traits>
#include <utility>

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

    std::unique_lock lk(_mutex);

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
        std::unique_lock lk(_mutex);

        _cv.wait(lk, [&] { return _state.load() == State::kShutdown; });
        invariant(_state.load() == State::kShutdown);
        return;
    }

    LOGV2_INFO(40441, "Stopping TopologyVersionObserver");

    // Wait for the thread to stop and steal it to the local stack
    auto thread = [&] {
        std::unique_lock lk(_mutex);

        // If we are still running, attempt to kill any opCtx
        if (_workerOpCtx) {
            ClientLock clientLk(_workerOpCtx->getClient());
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
    std::lock_guard<std::mutex> lk(_mutex);
    return _cache;
}

void TopologyVersionObserver::registerTopologyChangeObserver(TopologyChangeCallback cb) {
    std::lock_guard lk(_mutex);
    _callbacks.push_back(std::move(cb));
}

ReplSetConfig TopologyVersionObserver::getReplSetConfig() {
    return _replCoordinator->getConfig();
}

void TopologyVersionObserver::_handleTopologyUpdate(
    OperationContext* opCtx, boost::optional<TopologyVersion> topologyVersion) try {
    invariant(opCtx);

    LOGV2_DEBUG(4794600, 3, "Waiting for a topology change");

    {
        ScopeGuard cacheGuard([&] {
            // If we're not dismissed, reset the _cache.
            std::lock_guard lk(_mutex);
            _cache.reset();
        });

        invariant(_replCoordinator);

        // This function will return when our topology version is stale
        auto future = _replCoordinator->getHelloResponseFuture({}, topologyVersion);

        if (auto response = std::move(future).get(opCtx); response->isConfigSet()) {
            cacheGuard.dismiss();

            std::vector<TopologyChangeCallback> tmpCallbacks;

            std::unique_lock lk(_mutex);
            _cache = response;

            ON_BLOCK_EXIT([&] {
                lk.lock();
                _callbacks.insert(_callbacks.end(),
                                  std::make_move_iterator(tmpCallbacks.begin()),
                                  std::make_move_iterator(tmpCallbacks.end()));
            });

            tmpCallbacks.swap(_callbacks);
            lk.unlock();

            for (const auto& cb : tmpCallbacks) {
                cb(_replCoordinator->getConfig());
            }
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
    ThreadClient tc(kTopologyVersionObserverName, _serviceContext->getService());

    // This thread may be interrupted by replication state changes and this is safe because
    // _handleTopologyUpdate is the only place where an opCtx is used and already has logic for
    // handling exceptions. Any logic added to this thread that uses the opCtx must be able to
    // handle interrupts.

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
        std::lock_guard lk(_mutex);
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
            std::lock_guard lk(_mutex);
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
            std::lock_guard lk(_mutex);
            // Checking `_shouldShutdown` under the lock is necessary to ensure the shutdown
            // method can interrupt the new operation.
            if (_shouldShutdown.load())
                break;
            _workerOpCtx = opCtxHandle.get();
        }

        ON_BLOCK_EXIT([&] {
            // We're done with our opCtxHandle, unset _workerOpCtx.
            std::lock_guard lk(_mutex);
            _workerOpCtx = nullptr;
        });

        // Pause here so that we can force there to be an opCtx to be interrupted.
        topologyVersionObserverExpectsInterruption.pauseWhileSet();

        _handleTopologyUpdate(opCtxHandle.get(), getTopologyVersion());
    }
} catch (const ExceptionFor<ErrorCategory::ShutdownError>& e) {
    LOGV2_DEBUG(40443, 3, "Observer thread stopped due to shutdown", "error"_attr = e.toString());
}

}  // namespace repl
}  // namespace mongo
