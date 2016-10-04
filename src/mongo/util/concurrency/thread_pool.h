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

#include <deque>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/time_support.h"

namespace mongo {

class Status;

/**
 * A configurable thread pool, for general use.
 *
 * See the Options struct for information about how to configure an instance.
 */
class ThreadPool final : public ThreadPoolInterface {
    MONGO_DISALLOW_COPYING(ThreadPool);

public:
    /**
     * Structure used to configure an instance of ThreadPool.
     */
    struct Options {
        // Name of the thread pool. If this string is empty, the pool will be assigned a
        // name unique to the current process.
        std::string poolName;

        // Prefix used to name threads for logging purposes.
        //
        // An integer will be appended to this string to create the thread name for each thread in
        // the pool.  Warning, if you create two pools and give them the same threadNamePrefix, you
        // could have multiple threads that report the same name. If you leave this empty, the
        // prefix will be the pool name followed by a hyphen.
        std::string threadNamePrefix;

        // Minimum number of threads that must be in the pool.
        //
        // At least this many threads will be created at startup, and the pool will not reduce the
        // total number of threads below this threshold before shutdown.
        size_t minThreads = 1;

        // The pool will never grow to contain more than this many threads.
        size_t maxThreads = 8;

        // If the pool has had at least one idle thread for this much time, it may consider reaping
        // a thread.
        Milliseconds maxIdleThreadAge = Seconds{30};

        // This function is run before each worker thread begins consuming tasks.
        using OnCreateThreadFn = stdx::function<void(const std::string& threadName)>;
        OnCreateThreadFn onCreateThread = [](const std::string&) {};
    };

    /**
     * Structure used to return information about the thread pool via getStats().
     */
    struct Stats {
        // The options for the instance of the pool returning these stats.
        Options options;

        // The number of threads currently in the pool, idle or active.
        size_t numThreads;

        // The number of idle threads currently in the pool.
        size_t numIdleThreads;

        // The number of tasks waiting to be executed by the pool.
        size_t numPendingTasks;

        // The last time that no threads in the pool were idle.
        Date_t lastFullUtilizationDate;
    };

    /**
     * Constructs a thread pool, configured with the given "options".
     */
    explicit ThreadPool(Options options);

    ~ThreadPool() override;

    void startup() override;
    void shutdown() override;
    void join() override;
    Status schedule(Task task) override;

    /**
     * Blocks the caller until there are no pending tasks on this pool.
     *
     * It is legal to call this whether or not shutdown has been called, but if it is called
     * *before* shutdown() is called, there is no guarantee that there will still be no pending
     * tasks when the function returns.
     *
     * May be called multiple times, by multiple threads. May not be called by a task in the thread
     * pool.
     */
    void waitForIdle();

    /**
     * Returns statistics about the thread pool's utilization.
     */
    Stats getStats() const;

private:
    using TaskList = std::deque<Task>;
    using ThreadList = std::vector<stdx::thread>;

    /**
     * Representation of the stage of life of a thread pool.
     *
     * A pool starts out in the preStart state, and ends life in the shutdownComplete state.  Work
     * may only be scheduled in the preStart and running states. Threads may only be started in the
     * running state. In shutdownComplete, there are no remaining threads or pending tasks to
     * execute.
     *
     * Diagram of legal transitions:
     *
     * preStart -> running -> joinRequired -> joining -> shutdownComplete
     *        \               ^
     *         \_____________/
     */
    enum LifecycleState { preStart, running, joinRequired, joining, shutdownComplete };

    /**
     * This is the thread body for worker threads.  It is a static member function,
     * because late in its execution it is possible for the pool to have been destroyed.
     * As such, it is advisable to pass the pool pointer as an explicit argument, rather
     * than as the implicit "this" argument.
     */
    static void _workerThreadBody(ThreadPool* pool, const std::string& threadName);

    /**
     * Starts a worker thread, unless _options.maxThreads threads are already running or
     * _state is not running.
     */
    void _startWorkerThread_inlock();

    /**
     * This is the run loop of a worker thread, invoked by _workerThreadBody.
     */
    void _consumeTasks();

    /**
     * Implementation of shutdown once _mutex is locked.
     */
    void _shutdown_inlock();

    /**
     * Implementation of join once _mutex is owned by "lk".
     */
    void _join_inlock(stdx::unique_lock<stdx::mutex>* lk);

    /**
     * Executes one task from _pendingTasks. "lk" must own _mutex, and _pendingTasks must have at
     * least one entry.
     */
    void _doOneTask(stdx::unique_lock<stdx::mutex>* lk);

    /**
     * Changes the lifecycle state (_state) of the pool and wakes up any threads waiting for a state
     * change. Has no effect if _state == newState.
     */
    void _setState_inlock(LifecycleState newState);

    // These are the options with which the pool was configured at construction time.
    const Options _options;

    // Mutex guarding all non-const member variables.
    mutable stdx::mutex _mutex;

    // This variable represents the lifecycle state of the pool.
    //
    // Work may only be scheduled in states preStart and running, and only executes in states
    // running and shuttingDown.
    LifecycleState _state = preStart;

    // Condition signaled to indicate that there is work in the _pendingTasks queue, or
    // that the system is shutting down.
    stdx::condition_variable _workAvailable;

    // Condition signaled to indicate that there is no work in the _pendingTasks queue.
    stdx::condition_variable _poolIsIdle;

    // Condition variable signaled whenever _state changes.
    stdx::condition_variable _stateChange;

    // Queue of yet-to-be-executed tasks.
    TaskList _pendingTasks;

    // List of threads serving as the worker pool.
    ThreadList _threads;

    // Count of idle threads.
    size_t _numIdleThreads = 0;

    // Id counter for assigning thread names
    size_t _nextThreadId = 0;

    // The last time that _pendingTasks.size() grew to be at least _threads.size().
    Date_t _lastFullUtilizationDate;
};

}  // namespace mongo
