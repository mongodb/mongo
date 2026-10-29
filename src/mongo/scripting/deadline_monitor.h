// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

// IWYU pragma: no_include "cxxabi.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>

namespace mongo {

// Returns the current interrupt interval from the setParameter value
int getScriptingEngineInterruptInterval();

/**
 * DeadlineMonitor
 *
 * Monitors tasks which are required to complete before a deadline.  When
 * a deadline is started on a _Task*, either the deadline must be stopped,
 * or _Task::kill() will be called when the deadline arrives.
 *
 * Each instance of a DeadlineMonitor spawns a thread which waits for one of the
 * following conditions:
 *    - a task is added to the monitor
 *    - a task is removed from the monitor
 *    - the nearest deadline has arrived
 *
 * Ownership:
 * The _Task* must not be freed until the deadline has elapsed or stopDeadline()
 * has been called.
 *
 * NOTE: Each instance of this class spawns a new thread.  It is intended to be a stop-gap
 *       solution for simple deadline monitoring until a more robust solution can be
 *       implemented.
 *
 * NOTE: timing is based on wallclock time, which may not be precise.
 */
template <typename _Task>
class DeadlineMonitor {
    DeadlineMonitor(const DeadlineMonitor&) = delete;
    DeadlineMonitor& operator=(const DeadlineMonitor&) = delete;

public:
    DeadlineMonitor() {
        // NOTE(schwerin): Because _monitorThread takes a pointer to "this", all of the fields
        // of this instance must be initialized before the thread is created.  As a result, we
        // should not create the thread in the initializer list.  Creating it there leaves us
        // vulnerable to errors introduced by rearranging the order of fields in the class.
        _monitorThread = stdx::thread(&mongo::DeadlineMonitor<_Task>::deadlineMonitorThread, this);
    }

    ~DeadlineMonitor() {
        {
            // ensure the monitor thread has been stopped before destruction
            std::lock_guard<std::mutex> lk(_deadlineMutex);
            _inShutdown = true;
            _newDeadlineAvailable.notify_one();
        }
        _monitorThread.join();
    }

    /**
     * Start monitoring a task for deadline lapse.  User must call stopDeadline() before
     * deleting the task.  Note that stopDeadline() cannot be called from within the
     * kill() method.
     * @param   task        the task to kill()
     * @param   timeoutMs   number of milliseconds before the deadline expires
     */
    void startDeadline(_Task* const task, int64_t timeoutMs) {
        Date_t deadline;
        if (timeoutMs > 0) {
            deadline = Date_t::now() + Milliseconds(timeoutMs);
        } else {
            deadline = Date_t::max();
        }
        std::lock_guard<std::mutex> lk(_deadlineMutex);

        if (_tasks.find(task) == _tasks.end()) {
            _tasks.emplace(task, deadline);
        }

        // Notify the monitor thread when either the nearest deadline moves earlier, or this is the
        // first task in the queue.
        if (deadline < _nearestDeadlineWallclock || _tasks.size() == 1) {
            _nearestDeadlineWallclock = deadline;
            _newDeadlineAvailable.notify_one();
        }
    }

    /**
     * Stop monitoring a task.  Can be called multiple times, before or after a
     * deadline has expired (as long as the task remains allocated).
     * @return true  if the task was found and erased
     */
    bool stopDeadline(_Task* const task) {
        std::lock_guard<std::mutex> lk(_deadlineMutex);
        return _tasks.erase(task);
    }

private:
    /**
     * Main deadline monitor loop.  Waits on a condition variable until a task
     * is started, stopped, or the nearest deadline arrives.  If a deadline arrives,
     * _Task::kill() is invoked.
     */
    void deadlineMonitorThread() {
        setThreadName("DeadlineMonitor");
        std::unique_lock<std::mutex> lk(_deadlineMutex);
        Date_t lastInterruptCycle = Date_t::now();
        while (!_inShutdown) {
            // get the next interval to wait
            const Date_t now = Date_t::now();
            const auto interruptInterval = Milliseconds{getScriptingEngineInterruptInterval()};

            if (now - lastInterruptCycle > interruptInterval) {
                for (const auto& task : _tasks) {
                    if (task.first->isKillPending())
                        task.first->kill();
                }
                lastInterruptCycle = now;
            }

            // Wait for a task to be added, a deadline to expire, or the next interrupt cycle to
            // come due. Waking for the interrupt cycle even when a (possibly much later) task
            // deadline exists is what keeps the isKillPending() pass above periodic; that pass
            // is how kills recorded only on a task's OperationContext (killOp, maxTimeMS expiry,
            // client disconnect -- see SERVER-130767) reach a thread stuck inside JS.
            if (_nearestDeadlineWallclock > now) {
                MONGO_IDLE_THREAD_BLOCK;
                auto waitDeadline = _nearestDeadlineWallclock;
                if ((interruptInterval.count() > 0) && !_tasks.empty()) {
                    waitDeadline = std::min(waitDeadline, lastInterruptCycle + interruptInterval);
                }
                if (waitDeadline == Date_t::max()) {
                    _newDeadlineAvailable.wait(lk);
                } else {
                    _newDeadlineAvailable.wait_until(lk, waitDeadline.toSystemTimePoint());
                }
                continue;
            }

            // set the next interval to wait for deadline completion
            _nearestDeadlineWallclock = Date_t::max();
            auto i = _tasks.begin();
            while (i != _tasks.end()) {
                if (i->second < now) {
                    // deadline expired
                    i->first->kill();
                    _tasks.erase(i++);
                } else {
                    if (i->second < _nearestDeadlineWallclock) {
                        // nearest deadline seen so far
                        _nearestDeadlineWallclock = i->second;
                    }
                    ++i;
                }
            }
        }
    }

    using TaskDeadlineMap = stdx::unordered_map<_Task*, Date_t>;
    TaskDeadlineMap _tasks;  // map of running tasks with deadlines
    // protects all non-const members, except _monitorThread
    std::mutex _deadlineMutex;
    stdx::condition_variable _newDeadlineAvailable;    // Signaled for timeout, start and stop
    stdx::thread _monitorThread;                       // the deadline monitor thread
    Date_t _nearestDeadlineWallclock = Date_t::max();  // absolute time of the nearest deadline
    bool _inShutdown = false;
};

}  // namespace mongo
