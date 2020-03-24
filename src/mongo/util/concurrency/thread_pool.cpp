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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/util/concurrency/thread_pool.h"

#include "mongo/base/status.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/str.h"

#include <sstream>

namespace mongo {

namespace {

// Counter used to assign unique names to otherwise-unnamed thread pools.
AtomicWord<int> nextUnnamedThreadPoolId{1};

/**
 * Sets defaults and checks bounds limits on "options", and returns it.
 *
 * This method is just a helper for the ThreadPool constructor.
 */
ThreadPool::Options cleanUpOptions(ThreadPool::Options&& options) {
    if (options.poolName.empty()) {
        options.poolName = str::stream() << "ThreadPool" << nextUnnamedThreadPoolId.fetchAndAdd(1);
    }
    if (options.threadNamePrefix.empty()) {
        options.threadNamePrefix = str::stream() << options.poolName << '-';
    }
    if (options.maxThreads < 1) {
        LOGV2_FATAL(28702,
                    "Cannot create pool {poolName} with maximum number of threads of "
                    "{maxThreads} which is less than 1",
                    "Cannot create pool with maximum number of threads less than 1",
                    "poolName"_attr = options.poolName,
                    "maxThreads"_attr = options.maxThreads);
    }
    if (options.minThreads > options.maxThreads) {
        LOGV2_FATAL(28686,
                    "Cannot create pool {poolName} with minimum number of threads of "
                    "{minThreads} which is larger than the configured maximum of {maxThreads}",
                    "Cannot create pool with minimum number of threads larger than the "
                    "configured maximum",
                    "poolName"_attr = options.poolName,
                    "minThreads"_attr = options.minThreads,
                    "maxThreads"_attr = options.maxThreads);
    }
    return {std::move(options)};
}

}  // namespace

ThreadPool::ThreadPool(Options options) : _options(cleanUpOptions(std::move(options))) {}

ThreadPool::~ThreadPool() {
    stdx::unique_lock<Latch> lk(_mutex);
    _shutdown_inlock();
    if (shutdownComplete != _state) {
        _join_inlock(&lk);
    }

    if (shutdownComplete != _state) {
        LOGV2_FATAL(28704, "Failed to shutdown pool during destruction");
    }
    invariant(_threads.empty());
    invariant(_pendingTasks.empty());
}

void ThreadPool::startup() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_state != preStart) {
        LOGV2_FATAL(28698,
                    "Attempted to start pool {poolName}, but it has already started",
                    "Attempted to start pool that has already started",
                    "poolName"_attr = _options.poolName);
    }
    _setState_inlock(running);
    invariant(_threads.empty());
    const size_t numToStart =
        std::min(_options.maxThreads, std::max(_options.minThreads, _pendingTasks.size()));
    for (size_t i = 0; i < numToStart; ++i) {
        _startWorkerThread_inlock();
    }
}

void ThreadPool::shutdown() {
    stdx::lock_guard<Latch> lk(_mutex);
    _shutdown_inlock();
}

void ThreadPool::_shutdown_inlock() {
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

void ThreadPool::join() {
    stdx::unique_lock<Latch> lk(_mutex);
    _join_inlock(&lk);
}

void ThreadPool::_joinRetired_inlock() {
    while (!_retiredThreads.empty()) {
        auto& t = _retiredThreads.front();
        t.join();
        _options.onJoinRetiredThread(t);
        _retiredThreads.pop_front();
    }
}

void ThreadPool::_join_inlock(stdx::unique_lock<Latch>* lk) {
    _stateChange.wait(*lk, [this] {
        switch (_state) {
            case preStart:
                return false;
            case running:
                return false;
            case joinRequired:
                return true;
            case joining:
            case shutdownComplete:
                LOGV2_FATAL(28700,
                            "Attempted to join pool {poolName} more than once",
                            "Attempted to join pool more than once",
                            "poolName"_attr = _options.poolName);
        }
        MONGO_UNREACHABLE;
    });
    _setState_inlock(joining);
    ++_numIdleThreads;
    if (!_pendingTasks.empty()) {
        lk->unlock();
        _drainPendingTasks();
        lk->lock();
    }
    --_numIdleThreads;
    _joinRetired_inlock();
    ThreadList threadsToJoin;
    swap(threadsToJoin, _threads);
    lk->unlock();
    for (auto& t : threadsToJoin) {
        t.join();
    }
    lk->lock();
    invariant(_state == joining);
    _setState_inlock(shutdownComplete);
}

void ThreadPool::_drainPendingTasks() {
    // Tasks cannot be run inline because they can create OperationContexts and the join() caller
    // may already have one associated with the thread.
    stdx::thread cleanThread = stdx::thread([&] {
        const std::string threadName = str::stream()
            << _options.threadNamePrefix << _nextThreadId++;
        setThreadName(threadName);
        _options.onCreateThread(threadName);
        stdx::unique_lock<Latch> lock(_mutex);
        while (!_pendingTasks.empty()) {
            _doOneTask(&lock);
        }
    });
    cleanThread.join();
}

void ThreadPool::schedule(Task task) {
    stdx::unique_lock<Latch> lk(_mutex);

    switch (_state) {
        case joinRequired:
        case joining:
        case shutdownComplete: {
            auto status = Status(ErrorCodes::ShutdownInProgress,
                                 str::stream() << "Shutdown of thread pool " << _options.poolName
                                               << " in progress");

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

void ThreadPool::waitForIdle() {
    stdx::unique_lock<Latch> lk(_mutex);
    // If there are any pending tasks, or non-idle threads, the pool is not idle.
    while (!_pendingTasks.empty() || _numIdleThreads < _threads.size()) {
        _poolIsIdle.wait(lk);
    }
}

ThreadPool::Stats ThreadPool::getStats() const {
    stdx::lock_guard<Latch> lk(_mutex);
    Stats result;
    result.options = _options;
    result.numThreads = _threads.size();
    result.numIdleThreads = _numIdleThreads;
    result.numPendingTasks = _pendingTasks.size();
    result.lastFullUtilizationDate = _lastFullUtilizationDate;
    return result;
}

void ThreadPool::_workerThreadBody(ThreadPool* pool, const std::string& threadName) noexcept {
    setThreadName(threadName);
    pool->_options.onCreateThread(threadName);
    const auto poolName = pool->_options.poolName;
    LOGV2_DEBUG(23104,
                1,
                "Starting thread {threadName} in pool {poolName}",
                "Starting thread",
                "threadName"_attr = threadName,
                "poolName"_attr = poolName);
    pool->_consumeTasks();

    // At this point, another thread may have destroyed "pool", if this thread chose to detach
    // itself and remove itself from pool->_threads before releasing pool->_mutex.  Do not access
    // member variables of "pool" from here, on.
    //
    // This can happen if this thread decided to retire, got descheduled after removing itself
    // from _threads and calling detach(), and then the pool was deleted. When this thread resumes,
    // it is no longer safe to access "pool".
    LOGV2_DEBUG(23105,
                1,
                "Shutting down thread {threadName} in pool {poolName}",
                "Shutting down thread",
                "threadName"_attr = threadName,
                "poolName"_attr = poolName);
}

void ThreadPool::_consumeTasks() {
    stdx::unique_lock<Latch> lk(_mutex);
    while (_state == running) {
        if (_pendingTasks.empty()) {
            /**
             * Help with garbage collecting retired threads to:
             * * Reduce the memory overhead of _retiredThreads
             * * Expedite the shutdown process
             */
            _joinRetired_inlock();

            if (_threads.size() > _options.minThreads) {
                // Since there are more than minThreads threads, this thread may be eligible for
                // retirement. If it isn't now, it may be later, so it must put a time limit on how
                // long it waits on _workAvailable.
                const auto now = Date_t::now();
                const auto nextThreadRetirementDate =
                    _lastFullUtilizationDate + _options.maxIdleThreadAge;
                if (now >= nextThreadRetirementDate) {
                    _lastFullUtilizationDate = now;
                    LOGV2_DEBUG(23106,
                                1,
                                "Reaping this thread; next thread reaped no earlier than "
                                "{nextThreadRetirementDate}",
                                "Reaping this thread",
                                "nextThreadRetirementDate"_attr =
                                    _lastFullUtilizationDate + _options.maxIdleThreadAge);
                    break;
                }

                LOGV2_DEBUG(23107,
                            3,
                            "Not reaping this thread because the earliest retirement date is "
                            "{nextThreadRetirementDate}",
                            "Not reaping this thread",
                            "nextThreadRetirementDate"_attr = nextThreadRetirementDate);
                MONGO_IDLE_THREAD_BLOCK;
                _workAvailable.wait_until(lk, nextThreadRetirementDate.toSystemTimePoint());
            } else {
                // Since the number of threads is not more than minThreads, this thread is not
                // eligible for retirement. It is OK to sleep until _workAvailable is signaled,
                // because any new threads that put the number of total threads above minThreads
                // would be eligible for retirement once they had no work left to do.
                LOGV2_DEBUG(23108,
                            3,
                            "Waiting for work; the thread pool size is {numThreads}; the minimum "
                            "number of threads is {minThreads}",
                            "Waiting for work",
                            "numThreads"_attr = _threads.size(),
                            "minThreads"_attr = _options.minThreads);
                MONGO_IDLE_THREAD_BLOCK;
                _workAvailable.wait(lk);
            }
            continue;
        }

        _doOneTask(&lk);
    }

    // We still hold the lock, but this thread is retiring. If the whole pool is shutting down, this
    // thread lends a hand in draining the work pool and returns so it can be joined. Otherwise, it
    // falls through to the detach code, below.

    if (_state == joinRequired || _state == joining) {
        // Drain the leftover pending tasks.
        while (!_pendingTasks.empty()) {
            _doOneTask(&lk);
        }
        --_numIdleThreads;
        return;
    }
    --_numIdleThreads;

    if (_state != running) {
        LOGV2_FATAL_NOTRACE(28701,
                            "State of pool {poolName} is {actualState}, but expected "
                            "{expectedState}",
                            "Unexpected pool state",
                            "poolName"_attr = _options.poolName,
                            "actualState"_attr = static_cast<int32_t>(_state),
                            "expectedState"_attr = static_cast<int32_t>(running));
    }

    // This thread is ending because it was idle for too long.  Find self in _threads, remove self
    // from _threads, and add self to the list of retired threads.
    for (size_t i = 0; i < _threads.size(); ++i) {
        auto& t = _threads[i];
        if (t.get_id() != stdx::this_thread::get_id()) {
            continue;
        }
        std::swap(t, _threads.back());
        _retiredThreads.push_back(std::move(_threads.back()));
        _threads.pop_back();
        return;
    }

    std::ostringstream threadId;
    threadId << stdx::this_thread::get_id();
    LOGV2_FATAL_NOTRACE(28703,
                        "Could not find thread with id {threadId} in pool {poolName}",
                        "Could not find thread",
                        "threadId"_attr = threadId.str(),
                        "poolName"_attr = _options.poolName);
}

void ThreadPool::_doOneTask(stdx::unique_lock<Latch>* lk) noexcept {
    invariant(!_pendingTasks.empty());
    LOGV2_DEBUG(23109,
                3,
                "Executing a task on behalf of pool {poolName}",
                "Executing a task on behalf of pool",
                "poolName"_attr = _options.poolName);
    Task task = std::move(_pendingTasks.front());
    _pendingTasks.pop_front();
    --_numIdleThreads;
    lk->unlock();
    task(Status::OK());
    lk->lock();
    ++_numIdleThreads;
    if (_pendingTasks.empty() && _threads.size() == _numIdleThreads) {
        _poolIsIdle.notify_all();
    }
}

void ThreadPool::_startWorkerThread_inlock() {
    switch (_state) {
        case preStart:
            LOGV2_DEBUG(
                23110,
                1,
                "Not starting new thread in pool {poolName}, yet; waiting for "
                "startup() call",
                "Not starting new thread since the pool is still waiting for startup() call",
                "poolName"_attr = _options.poolName);
            return;
        case joinRequired:
        case joining:
        case shutdownComplete:
            LOGV2_DEBUG(23111,
                        1,
                        "Not starting new thread in pool {poolName}; shutting down",
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
                    "Not starting new thread in pool {poolName} because it already has "
                    "{maxThreads} threads, its maximum",
                    "Not starting new thread in pool since the pool is already full",
                    "poolName"_attr = _options.poolName,
                    "maxThreads"_attr = _options.maxThreads);
        return;
    }
    invariant(_threads.size() < _options.maxThreads);
    const std::string threadName = str::stream() << _options.threadNamePrefix << _nextThreadId++;
    try {
        _threads.emplace_back([this, threadName] { _workerThreadBody(this, threadName); });
        ++_numIdleThreads;
    } catch (const std::exception& ex) {
        LOGV2_ERROR(23113,
                    "Failed to start {threadName}; {numThreads} other thread(s) still running in "
                    "pool {poolName}; caught exception: {error}",
                    "Failed to start thread",
                    "threadName"_attr = threadName,
                    "numThreads"_attr = _threads.size(),
                    "poolName"_attr = _options.poolName,
                    "error"_attr = redact(ex.what()));
    }
}

void ThreadPool::_setState_inlock(const LifecycleState newState) {
    if (newState == _state) {
        return;
    }
    _state = newState;
    _stateChange.notify_all();
}

}  // namespace mongo
