/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class Status;

/**
 * Interface for a thread pool.
 */
class ThreadPoolInterface {
    MONGO_DISALLOW_COPYING(ThreadPoolInterface);

public:
    using Task = stdx::function<void()>;

    /**
     * Destroys a thread pool.
     *
     * The destructor may block if join() has not previously been called and returned.
     * It is fatal to destroy the pool while another thread is blocked on join().
     */
    virtual ~ThreadPoolInterface() = default;

    /**
     * Starts the thread pool. May be called at most once.
     */
    virtual void startup() = 0;

    /**
     * Signals the thread pool to shut down.  Returns promptly.
     *
     * After this call, the thread will return an error for subsequent calls to schedule().
     *
     * May be called by a task executing in the thread pool.  Call join() after calling shutdown()
     * to block until all tasks scheduled on the pool complete.
     */
    virtual void shutdown() = 0;

    /**
     * Blocks until the thread pool has fully shut down. Call at most once, and never from a task
     * inside the pool.
     */
    virtual void join() = 0;

    /**
     * Schedules "task" to run in the thread pool.
     *
     * Returns OK on success, ShutdownInProgress if shutdown() has already executed.
     *
     * It is safe to call this before startup(), but the scheduled task will not execute
     * until after startup() is called.
     */
    virtual Status schedule(Task task) = 0;

protected:
    ThreadPoolInterface() = default;
};

}  // namespace mongo
