// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace mongo {

class SessionScheduler {
public:
    /** The basic unit of work is a generic function that just runs. */
    using SessionTask = mongo::unique_function<void()>;

    /*
     * Session pool can accommodate N sessions.
     * This is not supposed to be a thread pool implementation for providing best performance. But
     * the aim is to simulate mongodb sessions, so the number of threads can be very high, depending
     * on the simulation. In general for each session there will be one thread. Each thread can only
     * read/process messages associated to its session.
     */
    SessionScheduler(size_t size = 1);
    ~SessionScheduler();

    void join();

    /** Add more command executors in case simple scheduling is not enough */
    void addSession();

    /*
     * Submit a task to execute inside the session pool. This means essentially submitting a new
     * command to execute. A future is returned in order to allow the called to wait for the
     * computation to finish.
     */
    template <typename F, typename... Args>
    void submit(F&& f, Args&&... args) {
        using mongo::unique_function;
        mongo::unique_function<void()> task(
            [this, f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
                try {
                    // Invoke the callable passed
                    std::invoke(f, args...);
                } catch (...) {
                    // ignore possible exception. The caller is responsible to handle errors. If
                    // they bubble up here, we will report them but the task will continue.
                    recordError(std::current_exception());
                }
            });
        {
            std::unique_lock<std::mutex> lock(_queueMutex);
            // Don't allow new tasks once the pool is stopped
            invariant(!_stop.load());
            _tasks.emplace(SessionTask(std::move(task)));
        }
        // notify all the threads that are listening on this condition variable. There should be
        // only one thread per session. So notify_one should suffice here, but for testing purposes
        // we need to be sure that all the working threads are awaken once a new task is submitted.
        _condition.notify_all();
    }

    bool containsExecutionErrors() const {
        return _hasRecordedErrors.load();
    }

    std::vector<std::exception_ptr> getExecutionErrors();

private:
    /**
     * Add a new worker into the session pool. This function is not thread safe and can only be
     * invoked by the constructor of the pool
     */
    void addWorker();
    /**
     * Execute a unit of work added to the session pool via submit. Return false when the pool has
     * ended and there is no more work to do.
     */
    bool executeTask();
    /**
     * Record a possible error encountered during task execution (eg std::bad_alloc or such)
     */
    void recordError(std::exception_ptr);

    std::vector<stdx::thread> _workers;   // Sessions threads (one per session is the default)
    std::queue<SessionTask> _tasks;       // The task queue
    std::mutex _queueMutex;               // Synchronizes access to the task queue
    stdx::condition_variable _condition;  // Used for thread synchronization
    Atomic<bool> _stop;                   // Indicates whether the pool is stopping

    std::mutex _errorMutex;                   // Synchronizes access to the error vector
    std::vector<std::exception_ptr> _errors;  // List of errors recorded during execution
    Atomic<bool> _hasRecordedErrors;  // Indicates whether the pool has recorded errors or not
};
}  // namespace mongo
