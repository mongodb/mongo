// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/status.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace [[MONGO_MOD_PARENT_PRIVATE]] mongo {
namespace repl {

/**
 * The DelayableTimeoutCallback is a utility class which allows a callback to be scheduled on an
 * executor at a given time, and then that time pushed back (later) arbitrarily often without
 * rescheduling the call on the executor.  The callback is never called with CallbackCanceled or
 * ShutdownInProgress.
 *
 * All methods are thread safe, though isActive() and getNextCall() may return stale information
 * if external synchronization is not used.  The callback is called without any locks held.
 */
class DelayableTimeoutCallback {
public:
    /**
     * Creates a DelayableTimeoutCallback with the given executor and callback function.  The
     * DelayableTimeoutCallback is inactive (the callback is not scheduled) when constructed.
     */
    DelayableTimeoutCallback(executor::TaskExecutor* executor,
                             executor::TaskExecutor::CallbackFn callback,
                             std::string timerName = std::string())
        : _executor(executor), _callback(std::move(callback)), _timerName(std::move(timerName)) {};

    ~DelayableTimeoutCallback();

    /**
     * If the timeout is scheduled, cancel it.  The callback function will not be called.
     */
    void cancel();

    /**
     * Schedule the timeout to occur at "when", regardless of if or when it is already scheduled.
     * If it is already scheduled to occur after "when", it is canceled and rescheduled.
     *
     * Returns status of the attempt to schedule on the executor.
     */
    Status scheduleAt(Date_t when);

    /**
     * Returns whether the callback is scheduled at all.
     */
    bool isActive() const;

    /**
     * Returns when the next call to the passed-in callback will be made, or Date_t() if inactive.
     */
    Date_t getNextCall() const;

protected:
    void _cancel(WithLock);
    Status _scheduleAt(WithLock, Date_t when);
    Status _delayUntil(WithLock, Date_t when);
    executor::TaskExecutor* _getExecutor() {
        return _executor;
    }

    mutable std::mutex _mutex;

private:
    void _handleTimeout(const executor::TaskExecutor::CallbackArgs& cbData);
    Status _reschedule(WithLock, Date_t when);

    executor::TaskExecutor* _executor;
    executor::TaskExecutor::CallbackHandle _cbHandle;
    const executor::TaskExecutor::CallbackFn _callback;
    Date_t _nextCall;

    // Timer name is used only for logging.
    const std::string _timerName;
};

/**
 * DelayableTimeoutCallbackWithJitter is a slight variation on DelayableTimeoutCallback
 * which adds some additional random time to delays.  Since the callback may be delayed at
 * intervals much shorter than the random time, this would naively result in the timeout
 * either being moved backwards often, or if we forbid moving it backwards, ending up quickly
 * moving to the maximum jitter (which isn't very random).  To avoid that, we only recompute
 * the jitter every maximum jitter interval -- e.g. if the max jitter is 10 seconds and we
 * add 3 seconds jitter at time T, we will add 3 seconds jitter to every subsequent call until
 * time T + 10.
 *
 * The typical purpose of the jitter is to prevent two timers receiving delay calls at the same
 * times from firing at the same time.
 *
 * Synchronization of the randomSource is up to the caller; it is provided externally to
 * avoid having a separate random number generator per timer.  The randomSource function will
 * be called with the maximum jitter value passed to scheduleAtWithJitter; it should return a value
 * in the range [0, maxJitter) or [0, maxJitter] depending on what you want the actual jitter range
 * to be.
 */
class DelayableTimeoutCallbackWithJitter : public DelayableTimeoutCallback {
public:
    using RandomSource = std::function<int64_t(WithLock, int64_t)>;

    DelayableTimeoutCallbackWithJitter(executor::TaskExecutor* executor,
                                       executor::TaskExecutor::CallbackFn callback,
                                       RandomSource randomSource,
                                       std::string timerName = std::string())
        : DelayableTimeoutCallback(executor, std::move(callback), timerName),
          _randomSource(std::move(randomSource)) {}

    Status scheduleAtWithJitter(WithLock, Date_t when, Milliseconds maxJitter);

private:
    void _resetRandomization(WithLock);
    void _updateJitter(WithLock, Milliseconds jitterUpperBound);

    RandomSource _randomSource;
    Date_t _lastRandomizationTime;
    Milliseconds _currentJitter;
};

}  // namespace repl
}  // namespace mongo
