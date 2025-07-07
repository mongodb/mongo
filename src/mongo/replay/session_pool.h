/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/functional.h"

#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <queue>
#include <vector>


namespace mongo {

class SessionPool {
public:
    /** The basic unit of work is a generic function that just runs. */
    using Task = mongo::unique_function<void()>;

    /*
     * Session pool can accommodate N sessions.
     * This is not supposed to be a thread pool implementation for providing best performance. But
     * the aim is to simulate mongodb sessions, so the number of threads can be very high, depending
     * on the simulation. In general for each session there will be one thread. Each thread can only
     * read/process messages associated to its session.
     */
    SessionPool(size_t size = 1);
    ~SessionPool();

    /*
     * Submit a task to execute inside the session pool. This means essentially submitting a new
     * command to execute. A future is returned in order to allow the called to wait for the
     * computation to finish.
     */
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> mongo::stdx::future<typename std::invoke_result_t<F, Args...>> {
        using namespace mongo;
        using ReturnType = typename std::invoke_result_t<F, Args...>;

        uassert(ErrorCodes::ReplayClientSessionSimulationError,
                "Cannot submit work to a stopped session pool",
                !_stop.load());
        uassert(ErrorCodes::ReplayClientSessionSimulationError,
                "The session pool has only one active session and it has recorded fatal errors, no "
                "more tasks can be submitted.",
                _sessionPoolSize.load() != 1 || !_hasRecordedErrors.load());

        stdx::packaged_task<void()> task(
            [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
                std::invoke(f, args...);
            });
        stdx::future<ReturnType> result = task.get_future();
        {
            stdx::unique_lock<stdx::mutex> lock(_queueMutex);
            _tasks.emplace(Task(std::move(task)));
        }
        // notify all the threads that are listening on this condition variable. There should be
        // only one thread per session. So notify_one should suffice here, but for testing purposes
        // we need to be sure that all the working threads are awaken once a new task is submitted.
        _condition.notify_all();
        return result;
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

    // TODO SERVER-106046 will address the possibility to have multiple dead threads in case of
    // multiple recordings.

    AtomicWord<size_t> _sessionPoolSize;  ///< Store total size of the pool

    std::vector<stdx::thread> _workers;   ///< Worker threads
    std::queue<Task> _tasks;              ///< The task queue
    stdx::mutex _queueMutex;              ///< Synchronizes access to the task queue
    stdx::condition_variable _condition;  ///< Used for thread synchronization
    AtomicWord<bool> _stop;               ///< Indicates whether the pool is stopping

    stdx::mutex _errorMutex;                  ///< Synchronizes access to the error vector
    std::vector<std::exception_ptr> _errors;  ///< List of errors recorded during execution
    AtomicWord<bool> _hasRecordedErrors;  ///< Indicates whether the pool has recorded errors or not
};
}  // namespace mongo
