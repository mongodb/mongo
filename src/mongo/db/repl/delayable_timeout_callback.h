/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/time_support.h"

namespace mongo {
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
        : _executor(executor), _callback(std::move(callback)), _timerName(std::move(timerName)){};

    ~DelayableTimeoutCallback();

    /**
     * If the timeout is scheduled, cancel it.  The callback function will not be called.
     */
    void cancel();

    /**
     * Schedule the timeout to occur at "when", regardless of if or when it is already scheduled.
     * If it is already scheduled to occur after "when", it is canceled and rescheduled
     *
     * Returns status of the attempt to schedule on the executor.
     */
    Status scheduleAt(Date_t when);

    /**
     * Schedule the timeout to occur at "when" if it is not scheduled or scheduled to occur before
     * "when".  If it is already scheduled to occur before "when", this call has no effect.
     *
     * Returns status of the attempt to schedule on the executor.
     */
    Status delayUntil(Date_t when);

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

    mutable Mutex _mutex = MONGO_MAKE_LATCH("DelayableTimeoutCallback");

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
 * be called with the maximum jitter value passed to delayUntilWithJitter; it should return
 * a value in the range [0, maxJitter) or [0, maxJitter] depending on what you want the
 * actual jitter range to be.
 */
class DelayableTimeoutCallbackWithJitter : public DelayableTimeoutCallback {
public:
    using RandomSource = std::function<int64_t(int64_t)>;

    DelayableTimeoutCallbackWithJitter(executor::TaskExecutor* executor,
                                       executor::TaskExecutor::CallbackFn callback,
                                       RandomSource randomSource,
                                       std::string timerName = std::string())
        : DelayableTimeoutCallback(executor, std::move(callback), timerName),
          _randomSource(std::move(randomSource)) {}

    Status scheduleAt(Date_t when);
    Status delayUntil(Date_t when);
    Status delayUntilWithJitter(Date_t when, Milliseconds maxJitter);

private:
    void _resetRandomization(WithLock);

    RandomSource _randomSource;
    Date_t _lastRandomizationTime;
    Milliseconds _currentJitter;
};

}  // namespace repl
}  // namespace mongo
