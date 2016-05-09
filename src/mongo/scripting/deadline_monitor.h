/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */
#pragma once

#include <cstdint>

#include "mongo/base/disallow_copying.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/time_support.h"

namespace mongo {

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
    MONGO_DISALLOW_COPYING(DeadlineMonitor);

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
            stdx::lock_guard<stdx::mutex> lk(_deadlineMutex);
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
        const auto deadline = Date_t::now() + Milliseconds(timeoutMs);
        stdx::lock_guard<stdx::mutex> lk(_deadlineMutex);

        _tasks[task] = deadline;

        if (deadline < _nearestDeadlineWallclock) {
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
        stdx::lock_guard<stdx::mutex> lk(_deadlineMutex);
        return _tasks.erase(task);
    }

private:
    /**
     * Main deadline monitor loop.  Waits on a condition variable until a task
     * is started, stopped, or the nearest deadline arrives.  If a deadline arrives,
     * _Task::kill() is invoked.
     */
    void deadlineMonitorThread() {
        stdx::unique_lock<stdx::mutex> lk(_deadlineMutex);
        while (!_inShutdown) {
            // get the next interval to wait
            const Date_t now = Date_t::now();

            // wait for a task to be added or a deadline to expire
            if (_nearestDeadlineWallclock > now) {
                if (_nearestDeadlineWallclock == Date_t::max()) {
                    _newDeadlineAvailable.wait(lk);
                } else {
                    _newDeadlineAvailable.wait_until(lk,
                                                     _nearestDeadlineWallclock.toSystemTimePoint());
                }
                continue;
            }

            // set the next interval to wait for deadline completion
            _nearestDeadlineWallclock = Date_t::max();
            typename TaskDeadlineMap::iterator i = _tasks.begin();
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

    typedef unordered_map<_Task*, Date_t> TaskDeadlineMap;
    TaskDeadlineMap _tasks;      // map of running tasks with deadlines
    stdx::mutex _deadlineMutex;  // protects all non-const members, except _monitorThread
    stdx::condition_variable _newDeadlineAvailable;    // Signaled for timeout, start and stop
    stdx::thread _monitorThread;                       // the deadline monitor thread
    Date_t _nearestDeadlineWallclock = Date_t::max();  // absolute time of the nearest deadline
    bool _inShutdown = false;
};

}  // namespace mongo
