/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <exception>
#include <list>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/functional.h"
#include "mongo/util/hierarchical_acquisition.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor


namespace mongo {

namespace {

using namespace fmt::literals;

// Counter used to assign unique names to otherwise-unnamed thread pools.
AtomicWord<int> nextUnnamedThreadPoolId{1};

std::string threadIdToString(stdx::thread::id id) {
    std::ostringstream oss;
    oss << id;
    return oss.str();
}

/**
 * Sets defaults and checks bounds limits on "options", and returns it.
 *
 * This method is just a helper for the ThreadPool constructor.
 */
ThreadPool::Options cleanUpOptions(ThreadPool::Options&& options) {
    if (options.poolName.empty()) {
        options.poolName = "ThreadPool{}"_format(nextUnnamedThreadPoolId.fetchAndAdd(1));
    }
    if (options.threadNamePrefix.empty()) {
        options.threadNamePrefix = "{}-"_format(options.poolName);
    }
    if (options.maxThreads < 1) {
        LOGV2_FATAL(28702,
                    "Cannot create pool with maximum number of threads less than 1",
                    "poolName"_attr = options.poolName,
                    "maxThreads"_attr = options.maxThreads);
    }
    if (options.minThreads > options.maxThreads) {
        LOGV2_FATAL(28686,
                    "Cannot create pool with minimum number of threads larger than the "
                    "configured maximum",
                    "poolName"_attr = options.poolName,
                    "minThreads"_attr = options.minThreads,
                    "maxThreads"_attr = options.maxThreads);
    }
    return {std::move(options)};
}

}  // namespace


// Public functions forwarded from ThreadPool.
class ThreadPool::Impl {
public:
    explicit Impl(Options options);
    ~Impl();
    void startup();
    void shutdown();
    void join();
    void schedule(Task task);
    void waitForIdle();
    Stats getStats() const;

private:
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

    /** The thread body for worker threads. */
    void _workerThreadBody(const std::string& threadName) noexcept;

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
    void _join_inlock(stdx::unique_lock<Latch>& lk);

    /**
     * Implementation of waitForIdle once _mutex is locked
     */
    void _waitForIdle(stdx::unique_lock<Latch>& lk);

    /**
     * Returns true when there are no _pendingTasks and all _threads are idle, including
     * _cleanUpThread.
     */
    bool _isPoolIdle(WithLock);

    /**
     * Runs the remaining tasks on a new thread as part of the join process, blocking until
     * complete.
     */
    void _drainPendingTasks(stdx::unique_lock<Latch>& lk);

    /**
     * Executes one task from _pendingTasks. "lk" must own _mutex, and _pendingTasks must have at
     * least one entry.
     */
    void _doOneTask(stdx::unique_lock<Latch>* lk) noexcept;

    /**
     * Changes the lifecycle state (_state) of the pool and wakes up any threads waiting for a state
     * change. Has no effect if _state == newState.
     */
    void _setState_inlock(LifecycleState newState);

    /**
     * Waits for all remaining retired threads to join.
     * If a thread's _workerThreadBody() were ever to attempt to reacquire
     * ThreadPool::_mutex after that thread had been added to _retiredThreads,
     * it could cause a deadlock.
     */
    void _joinRetired_inlock();

    // These are the options with which the pool was configured at construction time.
    const Options _options;

    // Mutex guarding all non-const member variables.
    mutable Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "ThreadPool::_mutex");

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
    std::deque<Task> _pendingTasks;

    // List of threads serving as the worker pool.
    std::list<stdx::thread> _threads;

    // List of threads that are retired and pending join
    std::list<stdx::thread> _retiredThreads;

    // Optional thread to drain the pending tasks upon join().
    boost::optional<stdx::thread> _cleanUpThread;

    // Count of idle threads.
    size_t _numIdleThreads = 0;

    // Id counter for assigning thread names
    size_t _nextThreadId = 0;

    // The last time that _pendingTasks.size() grew to be at least _threads.size().
    Date_t _lastFullUtilizationDate;
};

ThreadPool::Impl::Impl(Options options) : _options(cleanUpOptions(std::move(options))) {}

ThreadPool::Impl::~Impl() {
    stdx::unique_lock<Latch> lk(_mutex);
    _shutdown_inlock();
    if (_state != shutdownComplete) {
        _join_inlock(lk);
    }

    if (_state != shutdownComplete) {
        LOGV2_FATAL(28704, "Failed to shutdown pool during destruction");
    }
    invariant(_threads.empty());
    invariant(_pendingTasks.empty());
}

void ThreadPool::Impl::startup() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_state != preStart) {
        LOGV2_FATAL(28698,
                    "Attempted to start pool that has already started",
                    "poolName"_attr = _options.poolName);
    }
    _setState_inlock(running);
    invariant(_threads.empty());
    size_t numToStart = std::clamp(_pendingTasks.size(), _options.minThreads, _options.maxThreads);
    for (size_t i = 0; i < numToStart; ++i) {
        _startWorkerThread_inlock();
    }
}

void ThreadPool::Impl::shutdown() {
    stdx::lock_guard<Latch> lk(_mutex);
    _shutdown_inlock();
}

void ThreadPool::Impl::_shutdown_inlock() {
    switch (_state) {
        case preStart:
        case running:
            _setState_inlock(joinRequired);
            _workAvailable.notify_all();
            return;
        case joinRequired:
        case joining:
        case shutdownComplete:
            return;
    }
    MONGO_UNREACHABLE;
}

void ThreadPool::Impl::join() {
    stdx::unique_lock<Latch> lk(_mutex);
    _join_inlock(lk);
}

void ThreadPool::Impl::_joinRetired_inlock() {
    while (!_retiredThreads.empty()) {
        auto& t = _retiredThreads.front();
        t.join();
        if (_options.onJoinRetiredThread)
            _options.onJoinRetiredThread(t);
        _retiredThreads.pop_front();
    }
}

void ThreadPool::Impl::_join_inlock(stdx::unique_lock<Latch>& lk) {
    _stateChange.wait(lk, [this] { return _state != preStart && _state != running; });
    if (_state != joinRequired) {
        LOGV2_FATAL(
            28700, "Attempted to join pool more than once", "poolName"_attr = _options.poolName);
    }

    _setState_inlock(joining);
    if (!_pendingTasks.empty()) {
        _drainPendingTasks(lk);
    }
    _joinRetired_inlock();
    _waitForIdle(lk);
    auto threadsToJoin = std::exchange(_threads, {});
    _numIdleThreads = 0;
    lk.unlock();
    for (auto& t : threadsToJoin) {
        t.join();
    }
    lk.lock();
    invariant(_state == joining);
    _setState_inlock(shutdownComplete);
}

void ThreadPool::Impl::_drainPendingTasks(stdx::unique_lock<Latch>& lk) {
    // Tasks cannot be run inline because they can create OperationContexts and the join() caller
    // may already have one associated with the thread.
    ++_numIdleThreads;
    _cleanUpThread = stdx::thread([&] {
        const std::string threadName = "{}{}"_format(_options.threadNamePrefix, _nextThreadId++);
        setThreadName(threadName);
        if (_options.onCreateThread)
            _options.onCreateThread(threadName);
        stdx::unique_lock<Latch> lock(_mutex);
        while (!_pendingTasks.empty()) {
            _doOneTask(&lock);
        }
    });
    lk.unlock();

    _cleanUpThread->join();

    lk.lock();
    --_numIdleThreads;
    _cleanUpThread.reset();
}

void ThreadPool::Impl::schedule(Task task) {
    stdx::unique_lock<Latch> lk(_mutex);

    switch (_state) {
        case joinRequired:
        case joining:
        case shutdownComplete: {
            auto status =
                Status(ErrorCodes::ShutdownInProgress,
                       "Shutdown of thread pool {} in progress"_format(_options.poolName));

            lk.unlock();
            task(status);
            return;
        } break;

        case preStart:
        case running:
            break;
        default:
            MONGO_UNREACHABLE;
    }
    _pendingTasks.emplace_back(std::move(task));
    if (_state == preStart) {
        return;
    }
    if (_numIdleThreads < _pendingTasks.size()) {
        _startWorkerThread_inlock();
    }
    if (_numIdleThreads <= _pendingTasks.size()) {
        _lastFullUtilizationDate = Date_t::now();
    }
    _workAvailable.notify_one();
}

void ThreadPool::Impl::waitForIdle() {
    stdx::unique_lock<Latch> lk(_mutex);
    _waitForIdle(lk);
}

void ThreadPool::Impl::_waitForIdle(stdx::unique_lock<Latch>& lk) {
    _poolIsIdle.wait(lk, [&] { return _isPoolIdle(lk); });
}

bool ThreadPool::Impl::_isPoolIdle(WithLock) {
    return (_pendingTasks.empty() &&
            (_numIdleThreads >= _threads.size() + (_cleanUpThread ? 1 : 0)));
}

ThreadPool::Stats ThreadPool::Impl::getStats() const {
    stdx::lock_guard<Latch> lk(_mutex);
    Stats result;
    result.options = _options;
    result.numThreads = _threads.size() + (_cleanUpThread ? 1 : 0);
    result.numIdleThreads = _numIdleThreads;
    result.numPendingTasks = _pendingTasks.size();
    result.lastFullUtilizationDate = _lastFullUtilizationDate;
    return result;
}

void ThreadPool::Impl::_workerThreadBody(const std::string& threadName) noexcept {
    setThreadName(threadName);
    if (_options.onCreateThread)
        _options.onCreateThread(threadName);
    LOGV2_DEBUG(23104,
                1,
                "Starting thread",
                "threadName"_attr = threadName,
                "poolName"_attr = _options.poolName);
    _consumeTasks();
    LOGV2_DEBUG(23105,
                1,
                "Shutting down thread",
                "threadName"_attr = threadName,
                "poolName"_attr = _options.poolName);
}

void ThreadPool::Impl::_consumeTasks() {
    stdx::unique_lock<Latch> lk(_mutex);
    while (_state == running) {
        if (!_pendingTasks.empty()) {
            _doOneTask(&lk);
            continue;
        }

        // Help with garbage collecting retired threads to reduce the
        // memory overhead of _retiredThreads and expedite the shutdown
        // process.
        _joinRetired_inlock();

        boost::optional<Date_t> waitDeadline;

        if (_threads.size() > _options.minThreads) {
            // Since there are more than minThreads threads, this thread may be eligible for
            // retirement. If it isn't now, it may be later, so it must put a time limit on how
            // long it waits on _workAvailable.
            const auto now = Date_t::now();
            const auto nextRetirement = _lastFullUtilizationDate + _options.maxIdleThreadAge;
            if (now >= nextRetirement) {
                _lastFullUtilizationDate = now;
                LOGV2_DEBUG(23106,
                            1,
                            "Reaping this thread",
                            "nextThreadRetirementDate"_attr =
                                _lastFullUtilizationDate + _options.maxIdleThreadAge);
                break;
            }

            LOGV2_DEBUG(23107,
                        3,
                        "Not reaping this thread",
                        "nextThreadRetirementDate"_attr = nextRetirement);
            waitDeadline = nextRetirement;
        } else {
            // Since the number of threads is not more than minThreads, this thread is not
            // eligible for retirement. It is OK to sleep until _workAvailable is signaled,
            // because any new threads that put the number of total threads above minThreads
            // would be eligible for retirement once they had no work left to do.
            LOGV2_DEBUG(23108,
                        3,
                        "Waiting for work",
                        "numThreads"_attr = _threads.size(),
                        "minThreads"_attr = _options.minThreads);
        }

        auto wake = [&] {
            return _state != running || !_pendingTasks.empty();
        };
        MONGO_IDLE_THREAD_BLOCK;
        if (waitDeadline) {
            _workAvailable.wait_until(lk, waitDeadline->toSystemTimePoint(), wake);
        } else {
            _workAvailable.wait(lk, wake);
        }
    }

    // We still hold the lock, but this thread is retiring. If the whole pool is shutting down, this
    // thread lends a hand in draining the work pool and returns so it can be joined. Otherwise, it
    // falls through to the thread retirement code, below.

    if (_state == joinRequired || _state == joining) {
        // Drain the leftover pending tasks.
        while (!_pendingTasks.empty()) {
            _doOneTask(&lk);
        }
        return;
    }
    --_numIdleThreads;

    if (_state != running) {
        LOGV2_FATAL_NOTRACE(28701,
                            "Unexpected pool state",
                            "poolName"_attr = _options.poolName,
                            "actualState"_attr = static_cast<int32_t>(_state),
                            "expectedState"_attr = static_cast<int32_t>(running));
    }

    // This thread is ending because it was idle for too long.
    // Move self from _threads to _retiredThreads.
    auto selfId = stdx::this_thread::get_id();
    auto pos = std::find_if(
        _threads.begin(), _threads.end(), [&](auto&& t) { return t.get_id() == selfId; });
    if (pos == _threads.end()) {
        LOGV2_FATAL_NOTRACE(28703,
                            "Could not find thread",
                            "threadId"_attr = threadIdToString(selfId),
                            "poolName"_attr = _options.poolName);
    }
    _retiredThreads.splice(_retiredThreads.end(), _threads, pos);
}

void ThreadPool::Impl::_doOneTask(stdx::unique_lock<Latch>* lk) noexcept {
    invariant(!_pendingTasks.empty());
    LOGV2_DEBUG(
        23109, 3, "Executing a task on behalf of pool", "poolName"_attr = _options.poolName);
    Task task = std::move(_pendingTasks.front());
    _pendingTasks.pop_front();
    --_numIdleThreads;

    lk->unlock();
    // Run the task outside of the lock. Note that if the task throws, the task destructor will run
    // outside of the lock before the exception hits the noexcept boundary.
    task(Status::OK());

    // Reset the task and run the dtor before we reacquire the lock.
    task = {};
    lk->lock();

    ++_numIdleThreads;
    if (_isPoolIdle(*lk)) {
        _poolIsIdle.notify_all();
    }
}

void ThreadPool::Impl::_startWorkerThread_inlock() {
    switch (_state) {
        case preStart:
            LOGV2_DEBUG(
                23110,
                1,
                "Not starting new thread since the pool is still waiting for startup() call",
                "poolName"_attr = _options.poolName);
            return;
        case joinRequired:
        case joining:
        case shutdownComplete:
            LOGV2_DEBUG(23111,
                        1,
                        "Not starting new thread since the pool is shutting down",
                        "poolName"_attr = _options.poolName);
            return;
        case running:
            break;
        default:
            MONGO_UNREACHABLE;
    }
    if (_threads.size() == _options.maxThreads) {
        LOGV2_DEBUG(23112,
                    2,
                    "Not starting new thread in pool since the pool is already full",
                    "poolName"_attr = _options.poolName,
                    "maxThreads"_attr = _options.maxThreads);
        return;
    }
    invariant(_threads.size() < _options.maxThreads);
    std::string threadName = "{}{}"_format(_options.threadNamePrefix, _nextThreadId++);
    try {
        _threads.emplace_back([this, threadName] { _workerThreadBody(threadName); });
        ++_numIdleThreads;
    } catch (const std::exception& ex) {
        LOGV2_ERROR(23113,
                    "Failed to start thread",
                    "threadName"_attr = threadName,
                    "numThreads"_attr = _threads.size(),
                    "poolName"_attr = _options.poolName,
                    "error"_attr = redact(ex.what()));
    }
}

void ThreadPool::Impl::_setState_inlock(const LifecycleState newState) {
    if (newState == _state) {
        return;
    }
    _state = newState;
    _stateChange.notify_all();
}

// ========================================
// ThreadPool public functions that simply forward to the `_impl`.

ThreadPool::ThreadPool(Options options) : _impl{std::make_unique<Impl>(std::move(options))} {}

ThreadPool::~ThreadPool() = default;

void ThreadPool::startup() {
    _impl->startup();
}

void ThreadPool::shutdown() {
    _impl->shutdown();
}

void ThreadPool::join() {
    _impl->join();
}

void ThreadPool::schedule(Task task) {
    _impl->schedule(std::move(task));
}

void ThreadPool::waitForIdle() {
    _impl->waitForIdle();
}

ThreadPool::Stats ThreadPool::getStats() const {
    return _impl->getStats();
}

}  // namespace mongo
