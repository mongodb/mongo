// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


// IWYU pragma: no_include "cxxabi.h"
#include "mongo/db/repl/task_runner.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <mutex>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

namespace {
using UniqueLock = std::unique_lock<std::mutex>;
using LockGuard = std::lock_guard<std::mutex>;


/**
 * Runs a single task runner task.
 * Any exceptions thrown by the task will be logged and converted into a
 * next action of kCancel.
 */
TaskRunner::NextAction runSingleTask(const TaskRunner::Task& task,
                                     OperationContext* opCtx,
                                     const Status& status) {
    try {
        return task(opCtx, status);
    } catch (...) {
        LOGV2(21777,
              "Unhandled exception in task runner",
              "error"_attr = redact(exceptionToStatus()));
    }
    return TaskRunner::NextAction::kCancel;
}

}  // namespace

TaskRunner::TaskRunner(ThreadPool* threadPool)
    : _threadPool(threadPool), _active(false), _cancelRequested(false) {
    uassert(ErrorCodes::BadValue, "null thread pool", threadPool);
}

TaskRunner::~TaskRunner() {
    try {
        cancel();
        join();
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }
}

std::string TaskRunner::getDiagnosticString() const {
    std::lock_guard<std::mutex> lk(_mutex);
    str::stream output;
    output << "TaskRunner";
    output << " scheduled tasks: " << _tasks.size();
    output << " active: " << _active;
    output << " cancel requested: " << _cancelRequested;
    return output;
}

bool TaskRunner::isActive() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _active;
}

void TaskRunner::schedule(Task task) {
    invariant(task);

    std::lock_guard<std::mutex> lk(_mutex);

    _tasks.push_back(std::move(task));
    _condition.notify_all();

    if (_active) {
        return;
    }

    _threadPool->schedule([this](auto status) {
        invariant(status);
        _runTasks();
    });

    _active = true;
    _cancelRequested = false;
}

void TaskRunner::cancel() {
    std::lock_guard<std::mutex> lk(_mutex);
    _cancelRequested = true;
    _condition.notify_all();
}

void TaskRunner::join() {
    UniqueLock lk(_mutex);
    _condition.wait(lk, [this]() { return !_active; });
}

void TaskRunner::_runTasks() {
    // We initialize cc() because ServiceContextMongoD::_newOpCtx() expects cc() to be equal to the
    // client used to create the operation context.
    Client* client = &cc();
    if (AuthorizationManager::get(client->getService())->isAuthEnabled()) {
        AuthorizationSession::get(client)->grantInternalAuthorization();
    }

    while (Task task = _waitForNextTask()) {
        NextAction nextAction;

        {
            auto opCtx = client->makeOperationContext();
            nextAction = runSingleTask(task, opCtx.get(), Status::OK());
        }

        if (nextAction == NextAction::kCancel) {
            break;
        }
        // Release thread back to pool after disposing if no scheduled tasks in queue.
        if (nextAction == NextAction::kDisposeOperationContext ||
            nextAction == NextAction::kInvalid) {
            std::lock_guard<std::mutex> lk(_mutex);
            if (_tasks.empty()) {
                _finishRunTasks(lk);
                return;
            }
        }
    }

    std::list<Task> tasks;
    UniqueLock lk{_mutex};

    auto cancelTasks = [&]() {
        tasks.swap(_tasks);
        lk.unlock();
        // Cancel remaining tasks with a CallbackCanceled status.
        for (auto&& task : tasks) {
            runSingleTask(task,
                          nullptr,
                          Status(ErrorCodes::CallbackCanceled,
                                 "this task has been canceled by a previously invoked task"));
        }
        tasks.clear();
    };
    cancelTasks();

    lk.lock();
    _finishRunTasks(lk);
    cancelTasks();
}

void TaskRunner::_finishRunTasks(WithLock lk) {
    _active = false;
    _cancelRequested = false;
    _condition.notify_all();
}

TaskRunner::Task TaskRunner::_waitForNextTask() {
    UniqueLock lk(_mutex);

    while (_tasks.empty() && !_cancelRequested) {
        _condition.wait(lk);
    }

    if (_cancelRequested) {
        return Task();
    }

    Task task = std::move(_tasks.front());
    _tasks.pop_front();
    return task;
}

}  // namespace repl
}  // namespace mongo
