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
#include <mutex>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

namespace {
using UniqueLock = stdx::unique_lock<Latch>;
using LockGuard = stdx::lock_guard<Latch>;


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

// static
TaskRunner::Task TaskRunner::makeCancelTask() {
    return [](OperationContext* opCtx, const Status& status) {
        return NextAction::kCancel;
    };
}

TaskRunner::TaskRunner(ThreadPool* threadPool)
    : _threadPool(threadPool), _active(false), _cancelRequested(false) {
    uassert(ErrorCodes::BadValue, "null thread pool", threadPool);
}

TaskRunner::~TaskRunner() {
    DESTRUCTOR_GUARD(cancel(); join(););
}

std::string TaskRunner::getDiagnosticString() const {
    stdx::lock_guard<Latch> lk(_mutex);
    str::stream output;
    output << "TaskRunner";
    output << " scheduled tasks: " << _tasks.size();
    output << " active: " << _active;
    output << " cancel requested: " << _cancelRequested;
    return output;
}

bool TaskRunner::isActive() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _active;
}

void TaskRunner::schedule(Task task) {
    invariant(task);

    stdx::lock_guard<Latch> lk(_mutex);

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
    stdx::lock_guard<Latch> lk(_mutex);
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
        AuthorizationSession::get(client)->grantInternalAuthorization(client);
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
            stdx::lock_guard<Latch> lk(_mutex);
            if (_tasks.empty()) {
                _finishRunTasks_inlock();
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
    _finishRunTasks_inlock();
    cancelTasks();
}

void TaskRunner::_finishRunTasks_inlock() {
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
