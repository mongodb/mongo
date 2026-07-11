// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/executor/async_timer_interface.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

namespace mongo {
namespace executor {

/**
 * Underlying mock timer implementation.
 *
 * If asyncWait() is called after the timer has expired, the provided callback will
 * fire immediately.
 */
class AsyncTimerMockImpl {
public:
    explicit AsyncTimerMockImpl(Milliseconds expiration);

    /**
     * Cancel operations waiting on this timer, invoking their handlers with an
     * 'operation aborted' error code.
     */
    void cancel();

    /**
     * Perform an asynchronous wait on this timer. If timer has expired, callback
     * runs immediately, synchronously.
     */
    void asyncWait(AsyncTimerInterface::Handler handler);

    /**
     * Advance current time. If the given interval is greater than or equal to the
     * time left on the timer, expire and call callbacks now.
     */
    void fastForward(Milliseconds time);

    /**
     * Return the amount of time left on this timer.
     */
    Milliseconds timeLeft();

    /**
     * Reset the timer.
     */
    void expireAfter(Milliseconds expiration);

    /**
     * Returns the number of handlers on this timer.
     */
    int jobs();

private:
    void _callAllHandlers(std::error_code ec);

    std::mutex _mutex;
    Milliseconds _timeLeft;
    std::vector<AsyncTimerInterface::Handler> _handlers;
};

/**
 * A wrapper around a shared AsyncTimerMockImpl that AsyncTimerMockFactory holds.
 *
 * This abstraction is necessary to fulfill ownership requirements on both
 * sides of the factory: AsyncOp owns its AsyncTimerMock objects, and it can safely
 * destroy these without destroying the underlying AsyncTimerMockImpl objects
 * accessed by tests and introducing races.
 */
class [[MONGO_MOD_PUBLIC]] AsyncTimerMock : public AsyncTimerInterface {
public:
    AsyncTimerMock(std::shared_ptr<AsyncTimerMockImpl> timer);

    void cancel() override;

    void asyncWait(AsyncTimerInterface::Handler handler) override;

    void expireAfter(Milliseconds expiration) override;

private:
    // Unfortunate, but it makes the ownership model sane.
    std::shared_ptr<AsyncTimerMockImpl> _timer;
};

/**
 * A factory that returns mock timers to be used by tests.
 *
 * Shared pointers to timer impls are kept in a set here for access by tests,
 * and these are also passed into the returned AsyncTimerMock objects.
 */
class [[MONGO_MOD_PUBLIC]] AsyncTimerFactoryMock : public AsyncTimerFactoryInterface {
public:
    AsyncTimerFactoryMock() = default;

    /**
     * Create and return a new AsyncTimerMock object.
     */
    std::unique_ptr<AsyncTimerInterface> make(Milliseconds expiration) override;

    /**
     * Advance the current "time" and make stale timers expire.
     */
    void fastForward(Milliseconds time);

    /**
     * This will start at 0ms since the epoch and increment when fastForward is called.
     */
    Date_t now() override;

    /**
     * Returns the number of pending jobs across all timers.
     */
    int jobs();

private:
    std::recursive_mutex _timersMutex;  // NOLINT
    stdx::unordered_set<std::shared_ptr<AsyncTimerMockImpl>> _timers;
    Milliseconds _curTime;
};

}  // namespace executor
}  // namespace mongo
