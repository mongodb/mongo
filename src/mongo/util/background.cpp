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


// IWYU pragma: no_include "cxxabi.h"
#include "mongo/util/background.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <cstddef>
#include <exception>
#include <mutex>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {

class PeriodicTaskRunner : public BackgroundJob {
public:
    // Tasks are expected to finish reasonably quickly, so when a task run takes longer
    // than `kMinLog`, the verbosity of its summary log statement is upgraded from 3 to 0.
    static constexpr auto kMinLog = Milliseconds(100);

    PeriodicTaskRunner() : _shutdownRequested(false) {}

    void add(PeriodicTask* task);
    void remove(PeriodicTask* task);

    Status stop(int gracePeriodMillis);

private:
    std::string name() const override {
        return "PeriodicTaskRunner";
    }

    void run() override;

    // Runs all registered tasks. You must hold _mutex to call this function.
    void _runTasks();

    // Runs one task to completion, and optionally reports timing. You must hold _mutex
    // to call this function.
    void _runTask(PeriodicTask* task);

    // _mutex protects the _shutdownRequested flag and the _tasks vector.
    stdx::mutex _mutex;

    // The condition variable is used to sleep for the interval between task
    // executions, and is notified when the _shutdownRequested flag is toggled.
    stdx::condition_variable _cond;

    // Used to break the loop. You should notify _cond after changing this to true
    // so that shutdown proceeds promptly.
    bool _shutdownRequested;

    // The PeriodicTasks contained in this vector are NOT owned by the
    // PeriodicTaskRunner, and are not deleted. The vector never shrinks, removed Tasks
    // have their entry overwritten with NULL.
    std::vector<PeriodicTask*> _tasks;
};

stdx::mutex* runnerMutex() {
    static stdx::mutex mutex;
    return &mutex;
}

// A scoped lock like object that only locks/unlocks the mutex if it exists.
class ConditionalScopedLock {
public:
    ConditionalScopedLock(stdx::mutex* mutex) : _mutex(mutex) {
        if (_mutex)
            _mutex->lock();
    }
    ~ConditionalScopedLock() {
        if (_mutex)
            _mutex->unlock();
    }

private:
    stdx::mutex* const _mutex;
};

// The unique PeriodicTaskRunner, also zero-initialized.
PeriodicTaskRunner* runner = nullptr;

// The runner is never re-created once it has been destroyed.
bool runnerDestroyed = false;

}  // namespace

// both the BackgroundJob and the internal thread point to JobStatus
struct BackgroundJob::JobStatus {
    JobStatus() : state(NotStarted) {}

    stdx::mutex mutex;
    stdx::condition_variable done;
    State state;
};

BackgroundJob::BackgroundJob(bool selfDelete) : _selfDelete(selfDelete), _status(new JobStatus) {}

BackgroundJob::~BackgroundJob() {}

void BackgroundJob::jobBody() {
    const std::string threadName = name();
    if (!threadName.empty()) {
        setThreadName(threadName);
    }

    LOGV2_DEBUG(23098, 1, "BackgroundJob starting", "threadName"_attr = threadName);

    run();

    // We must cache this value so that we can use it after we leave the following scope.
    const bool selfDelete = _selfDelete;

    {
        // It is illegal to access any state owned by this BackgroundJob after leaving this
        // scope, with the exception of the call to 'delete this' below.
        stdx::unique_lock<stdx::mutex> l(_status->mutex);
        _status->state = Done;
        _status->done.notify_all();
    }

    if (selfDelete)
        delete this;
}

void BackgroundJob::go() {
    stdx::unique_lock<stdx::mutex> l(_status->mutex);
    massert(17234,
            str::stream() << "backgroundJob already running: " << name(),
            _status->state != Running);

    // If the job is already 'done', for instance because it was cancelled or already
    // finished, ignore additional requests to run the job.
    if (_status->state == NotStarted) {
        stdx::thread{[this] {
            jobBody();
        }}.detach();
        _status->state = Running;
    }
}

Status BackgroundJob::cancel() {
    stdx::unique_lock<stdx::mutex> l(_status->mutex);

    if (_status->state == Running)
        return Status(ErrorCodes::IllegalOperation, "Cannot cancel a running BackgroundJob");

    if (_status->state == NotStarted) {
        _status->state = Done;
        _status->done.notify_all();
    }

    return Status::OK();
}

bool BackgroundJob::wait(unsigned msTimeOut) {
    MONGO_verify(!_selfDelete);  // you cannot call wait on a self-deleting job
    const auto deadline = Date_t::now() + Milliseconds(msTimeOut);
    stdx::unique_lock<stdx::mutex> l(_status->mutex);
    while (_status->state != Done) {
        if (msTimeOut) {
            if (stdx::cv_status::timeout ==
                _status->done.wait_until(l, deadline.toSystemTimePoint()))
                return false;
        } else {
            _status->done.wait(l);
        }
    }
    return true;
}

BackgroundJob::State BackgroundJob::getState() const {
    stdx::unique_lock<stdx::mutex> l(_status->mutex);
    return _status->state;
}

bool BackgroundJob::running() const {
    stdx::unique_lock<stdx::mutex> l(_status->mutex);
    return _status->state == Running;
}

// -------------------------

PeriodicTask::PeriodicTask() {
    ConditionalScopedLock lock(runnerMutex());
    if (runnerDestroyed)
        return;

    if (!runner)
        runner = new PeriodicTaskRunner;

    runner->add(this);
}

PeriodicTask::~PeriodicTask() {
    ConditionalScopedLock lock(runnerMutex());
    if (runnerDestroyed || !runner)
        return;

    runner->remove(this);
}

void PeriodicTask::startRunningPeriodicTasks() {
    ConditionalScopedLock lock(runnerMutex());
    if (runnerDestroyed)
        return;

    if (!runner)
        runner = new PeriodicTaskRunner;

    runner->go();
}

Status PeriodicTask::stopRunningPeriodicTasks(int gracePeriodMillis) {
    ConditionalScopedLock lock(runnerMutex());

    Status status = Status::OK();
    if (runnerDestroyed || !runner)
        return status;

    runner->cancel().transitional_ignore();
    status = runner->stop(gracePeriodMillis);

    if (status.isOK()) {
        delete runner;
        runnerDestroyed = true;
    }

    return status;
}

void PeriodicTaskRunner::add(PeriodicTask* task) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _tasks.push_back(task);
}

void PeriodicTaskRunner::remove(PeriodicTask* task) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    for (size_t i = 0; i != _tasks.size(); i++) {
        if (_tasks[i] == task) {
            _tasks[i] = nullptr;
            break;
        }
    }
}

Status PeriodicTaskRunner::stop(int gracePeriodMillis) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _shutdownRequested = true;
        _cond.notify_one();
    }

    if (!wait(gracePeriodMillis)) {
        return Status(ErrorCodes::ExceededTimeLimit,
                      "Grace period expired while waiting for PeriodicTasks to terminate");
    }
    return Status::OK();
}

void PeriodicTaskRunner::run() {
    // Use a shorter cycle time in debug mode to help catch race conditions.
    const Seconds waitTime(kDebugBuild ? 5 : 60);

    stdx::unique_lock<stdx::mutex> lock(_mutex);
    while (!_shutdownRequested) {
        {
            MONGO_IDLE_THREAD_BLOCK;
            if (stdx::cv_status::timeout != _cond.wait_for(lock, waitTime.toSystemDuration()))
                continue;
        }
        _runTasks();
    }
}

void PeriodicTaskRunner::_runTasks() {
    const size_t size = _tasks.size();
    for (size_t i = 0; i != size; ++i)
        if (PeriodicTask* const task = _tasks[i])
            _runTask(task);
}

void PeriodicTaskRunner::_runTask(PeriodicTask* const task) {
    Timer timer;

    const std::string taskName = task->taskName();

    try {
        task->taskDoWork();
    } catch (const std::exception& e) {
        LOGV2_ERROR(
            23100, "Task failed", "taskName"_attr = taskName, "error"_attr = redact(e.what()));
    } catch (...) {
        LOGV2_ERROR(23101, "Task failed with unknown error", "taskName"_attr = taskName);
    }

    const auto duration = timer.elapsed();

    LOGV2_DEBUG(23099,
                duration <= kMinLog ? 3 : 0,
                "Task finished",
                "taskName"_attr = taskName,
                "duration"_attr = duration_cast<Milliseconds>(duration));
}

}  // namespace mongo
