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

#include "mongo/util/duration.h"
#include "mongo/util/inline_memory.h"

#include <cstddef>
#include <list>
#include <memory>

namespace mongo {

class OperationContext;

class OperationCPUTimer;

/**
 * Allocates and tracks CPU timers for an OperationContext.
 */
class OperationCPUTimers {
    friend class OperationCPUTimer;

public:
    virtual ~OperationCPUTimers() = default;

    /**
     * Returns `nullptr` if the platform does not support tracking of CPU consumption.
     */
    static OperationCPUTimers* get(OperationContext*);

    /**
     * Returns a timer bound to this OperationContext and the threads that it runs on. Timers
     * created from this function may safely outlive the OperationCPUTimers container and the
     * OperationContext, but only to simplify destruction ordering problems.
     */
    virtual OperationCPUTimer makeTimer() = 0;

    /**
     * Attaches the current thread.
     * Must be called when no thread is attached.
     */
    virtual void onThreadAttach() = 0;

    /**
     * Detaches the current thread.
     * Must be called from the thread that is currently attached.
     */
    virtual void onThreadDetach() = 0;

    /**
     * Returns the number of running timers.
     */
    virtual size_t runningCount() const = 0;

protected:
    /**
     * Returns the cpu thread time accumulated so far for this thread. Excludes the time when the
     * thread is detached.
     * Must be called from the thread that is currently attached.
     */
    virtual Nanoseconds _getOperationThreadTime() const = 0;

    virtual void _onTimerStart() = 0;

    virtual void _onTimerStop() = 0;
};

/**
 * Implements the CPU timer for platforms that support CPU consumption tracking. Consider the
 * following when using the timer:
 *
 * All methods may only be invoked on the thread associated with the operation.
 *
 * To access the timer, the operation must be associated with a client, and the client must be
 * attached to the current thread.
 *
 * The timer is initially stopped, measures elapsed time between the invocations of `start()`
 * and `stop()`, and resets on consequent invocations of `start()`.
 *
 * To reset a timer, it should be stopped first and then started again.
 *
 * The timer is paused when the operation's client is detached from the current thread, and will
 * not resume until the client is reattached to a thread.
 */
class OperationCPUTimer {
public:
    explicit OperationCPUTimer(OperationCPUTimers** timers);
    ~OperationCPUTimer();

    Nanoseconds getElapsed() const;

    void start();
    void stop();

private:
    OperationCPUTimers* getTimers() {
        return *_timers;
    }

    const OperationCPUTimers* getTimers() const {
        return *_timers;
    }

    // Weak reference to OperationContext-owned tracked list of timers. The Timers container can be
    // destructed before this Timer.
    OperationCPUTimers** _timers;

    // Whether the timer is running.
    bool _timerIsRunning;

    // Total time adjustment accrued so far. Includes the time when timer was started (as negative)
    // and time when timer was stopped (as positive).
    Nanoseconds _elapsedAdjustment;
};

}  // namespace mongo
