// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/duration.h"
#include "mongo/util/inline_memory.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <list>
#include <memory>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
