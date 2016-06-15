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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/util/concurrency/thread_pool.h"

#include "mongo/base/status.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

// Counter used to assign unique names to otherwise-unnamed thread pools.
AtomicInt32 nextUnnamedThreadPoolId{1};

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
        severe() << "Tried to create pool " << options.poolName << " with a maximum of "
                 << options.maxThreads << " but the maximum must be at least 1";
        fassertFailed(28702);
    }
    if (options.minThreads > options.maxThreads) {
        severe() << "Tried to create pool " << options.poolName << " with a minimum of "
                 << options.minThreads << " which is more than the configured maximum of "
                 << options.maxThreads;
        fassertFailed(28686);
    }
    return options;
}

}  // namespace

ThreadPool::ThreadPool(Options options) : _options(cleanUpOptions(std::move(options))) {}

ThreadPool::~ThreadPool() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _shutdown_inlock();
    if (shutdownComplete != _state) {
        _join_inlock(&lk);
    }

    if (shutdownComplete != _state) {
        severe() << "Failed to shutdown pool during destruction";
        fassertFailed(28704);
    }
    invariant(_threads.empty());
    invariant(_pendingTasks.empty());
}

void ThreadPool::startup() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_state != preStart) {
        severe() << "Attempting to start pool " << _options.poolName
                 << ", but it has already started";
        fassertFailed(28698);
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
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
    try {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _join_inlock(&lk);
    } catch (...) {
        std::terminate();
    }
}

void ThreadPool::_join_inlock(stdx::unique_lock<stdx::mutex>* lk) {
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
                severe() << "Attempted to join pool " << _options.poolName << " more than once";
                fassertFailed(28700);
        }
        MONGO_UNREACHABLE;
    });
    _setState_inlock(joining);
    ++_numIdleThreads;
    while (!_pendingTasks.empty()) {
        _doOneTask(lk);
    }
    --_numIdleThreads;
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

Status ThreadPool::schedule(Task task) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    switch (_state) {
        case joinRequired:
        case joining:
        case shutdownComplete:
            return Status(ErrorCodes::ShutdownInProgress,
                          str::stream() << "Shutdown of thread pool " << _options.poolName
                                        << " in progress");
        case preStart:
        case running:
            break;
        default:
            MONGO_UNREACHABLE;
    }
    _pendingTasks.emplace_back(std::move(task));
    if (_state == preStart) {
        return Status::OK();
    }
    if (_numIdleThreads < _pendingTasks.size()) {
        _startWorkerThread_inlock();
    }
    if (_numIdleThreads <= _pendingTasks.size()) {
        _lastFullUtilizationDate = Date_t::now();
    }
    _workAvailable.notify_one();
    return Status::OK();
}

void ThreadPool::waitForIdle() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    // If there are any pending tasks, or non-idle threads, the pool is not idle.
    while (!_pendingTasks.empty() || _numIdleThreads < _threads.size()) {
        _poolIsIdle.wait(lk);
    }
}

ThreadPool::Stats ThreadPool::getStats() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    Stats result;
    result.options = _options;
    result.numThreads = _threads.size();
    result.numIdleThreads = _numIdleThreads;
    result.numPendingTasks = _pendingTasks.size();
    result.lastFullUtilizationDate = _lastFullUtilizationDate;
    return result;
}

void ThreadPool::_workerThreadBody(ThreadPool* pool, const std::string& threadName) {
    setThreadName(threadName);
    pool->_options.onCreateThread(threadName);
    const auto poolName = pool->_options.poolName;
    LOG(1) << "starting thread in pool " << poolName;
    try {
        pool->_consumeTasks();
    } catch (...) {
        severe() << "Exception reached top of stack in thread pool " << poolName;
        std::terminate();
    }

    // At this point, another thread may have destroyed "pool", if this thread chose to detach
    // itself and remove itself from pool->_threads before releasing pool->_mutex.  Do not access
    // member variables of "pool" from here, on.
    //
    // This can happen if this thread decided to retire, got descheduled after removing itself
    // from _threads and calling detach(), and then the pool was deleted. When this thread resumes,
    // it is no longer safe to access "pool".
    LOG(1) << "shutting down thread in pool " << poolName;
}

void ThreadPool::_consumeTasks() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    while (_state == running) {
        if (_pendingTasks.empty()) {
            if (_threads.size() > _options.minThreads) {
                // Since there are more than minThreads threads, this thread may be eligible for
                // retirement. If it isn't now, it may be later, so it must put a time limit on how
                // long it waits on _workAvailable.
                const auto now = Date_t::now();
                const auto nextThreadRetirementDate =
                    _lastFullUtilizationDate + _options.maxIdleThreadAge;
                if (now >= nextThreadRetirementDate) {
                    _lastFullUtilizationDate = now;
                    LOG(1) << "Reaping this thread; next thread reaped no earlier than "
                           << _lastFullUtilizationDate + _options.maxIdleThreadAge;
                    break;
                }

                LOG(3) << "Not reaping because the earliest retirement date is "
                       << nextThreadRetirementDate;
                _workAvailable.wait_until(lk, nextThreadRetirementDate.toSystemTimePoint());
            } else {
                // Since the number of threads is not more than minThreads, this thread is not
                // eligible for retirement. It is OK to sleep until _workAvailable is signaled,
                // because any new threads that put the number of total threads above minThreads
                // would be eligible for retirement once they had no work left to do.
                LOG(3) << "waiting for work; I am one of " << _threads.size() << " thread(s);"
                       << " the minimum number of threads is " << _options.minThreads;
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
        severe() << "State of pool " << _options.poolName << " is " << static_cast<int32_t>(_state)
                 << ", but expected " << static_cast<int32_t>(running);
        fassertFailedNoTrace(28701);
    }

    // This thread is ending because it was idle for too long.  Find self in _threads, remove self
    // from _threads, detach self.
    for (size_t i = 0; i < _threads.size(); ++i) {
        auto& t = _threads[i];
        if (t.get_id() != stdx::this_thread::get_id()) {
            continue;
        }
        t.detach();
        t.swap(_threads.back());
        _threads.pop_back();
        return;
    }
    severe().stream() << "Could not find this thread, with id " << stdx::this_thread::get_id()
                      << " in pool " << _options.poolName;
    fassertFailedNoTrace(28703);
}

void ThreadPool::_doOneTask(stdx::unique_lock<stdx::mutex>* lk) {
    invariant(!_pendingTasks.empty());
    try {
        LOG(3) << "Executing a task on behalf of pool " << _options.poolName;
        Task task = std::move(_pendingTasks.front());
        _pendingTasks.pop_front();
        --_numIdleThreads;
        lk->unlock();
        task();
        lk->lock();
        ++_numIdleThreads;
        if (_pendingTasks.empty() && _threads.size() == _numIdleThreads) {
            _poolIsIdle.notify_all();
        }
    } catch (...) {
        severe() << "Exception escaped task in thread pool " << _options.poolName;
        std::terminate();
    }
}

void ThreadPool::_startWorkerThread_inlock() {
    switch (_state) {
        case preStart:
            LOG(1) << "Not starting new thread in pool " << _options.poolName
                   << ", yet; waiting for startup() call";
            return;
        case joinRequired:
        case joining:
        case shutdownComplete:
            LOG(1) << "Not starting new thread in pool " << _options.poolName
                   << " while shutting down";
            return;
        case running:
            break;
        default:
            MONGO_UNREACHABLE;
    }
    if (_threads.size() == _options.maxThreads) {
        LOG(2) << "Not starting new thread in pool " << _options.poolName
               << " because it already has " << _options.maxThreads << ", its maximum";
        return;
    }
    invariant(_threads.size() < _options.maxThreads);
    const std::string threadName = str::stream() << _options.threadNamePrefix << _nextThreadId++;
    try {
        _threads.emplace_back(stdx::bind(&ThreadPool::_workerThreadBody, this, threadName));
        ++_numIdleThreads;
    } catch (const std::exception& ex) {
        error() << "Failed to start " << threadName << "; " << _threads.size()
                << " other thread(s) still running in pool " << _options.poolName
                << "; caught exception: " << ex.what();
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
