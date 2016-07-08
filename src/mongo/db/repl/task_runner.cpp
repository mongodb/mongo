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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/task_runner.h"

#include <memory>

#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/old_thread_pool.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

namespace {
using UniqueLock = stdx::unique_lock<stdx::mutex>;
using LockGuard = stdx::lock_guard<stdx::mutex>;


/**
 * Runs a single task runner task.
 * Any exceptions thrown by the task will be logged and converted into a
 * next action of kCancel.
 */
TaskRunner::NextAction runSingleTask(const TaskRunner::Task& task,
                                     OperationContext* txn,
                                     const Status& status) {
    try {
        return task(txn, status);
    } catch (...) {
        log() << "Unhandled exception in task runner: " << exceptionToStatus();
    }
    return TaskRunner::NextAction::kCancel;
}

}  // namespace

// static
TaskRunner::Task TaskRunner::makeCancelTask() {
    return [](OperationContext* txn, const Status& status) { return NextAction::kCancel; };
}

TaskRunner::TaskRunner(OldThreadPool* threadPool)
    : _threadPool(threadPool), _active(false), _cancelRequested(false) {
    uassert(ErrorCodes::BadValue, "null thread pool", threadPool);
}

TaskRunner::~TaskRunner() {
    DESTRUCTOR_GUARD(cancel(); join(););
}

std::string TaskRunner::getDiagnosticString() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    str::stream output;
    output << "TaskRunner";
    output << " scheduled tasks: " << _tasks.size();
    output << " active: " << _active;
    output << " cancel requested: " << _cancelRequested;
    return output;
}

bool TaskRunner::isActive() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _active;
}

void TaskRunner::schedule(const Task& task) {
    invariant(task);

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _tasks.push_back(task);
    _condition.notify_all();

    if (_active) {
        return;
    }

    _threadPool->schedule(stdx::bind(&TaskRunner::_runTasks, this));

    _active = true;
    _cancelRequested = false;
}

void TaskRunner::cancel() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _cancelRequested = true;
    _condition.notify_all();
}

void TaskRunner::join() {
    UniqueLock lk(_mutex);
    _condition.wait(lk, [this]() { return !_active; });
}

void TaskRunner::_runTasks() {
    Client* client = nullptr;
    ServiceContext::UniqueOperationContext txn;

    while (Task task = _waitForNextTask()) {
        if (!txn) {
            if (!client) {
                // We initialize cc() because ServiceContextMongoD::_newOpCtx() expects cc()
                // to be equal to the client used to create the operation context.
                Client::initThreadIfNotAlready();
                client = &cc();
                if (getGlobalAuthorizationManager()->isAuthEnabled()) {
                    AuthorizationSession::get(client)->grantInternalAuthorization();
                }
            }
            txn = client->makeOperationContext();
        }

        NextAction nextAction = runSingleTask(task, txn.get(), Status::OK());

        if (nextAction != NextAction::kKeepOperationContext) {
            txn.reset();
        }

        if (nextAction == NextAction::kCancel) {
            break;
        }
        // Release thread back to pool after disposing if no scheduled tasks in queue.
        if (nextAction == NextAction::kDisposeOperationContext ||
            nextAction == NextAction::kInvalid) {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            if (_tasks.empty()) {
                _finishRunTasks_inlock();
                return;
            }
        }
    }
    txn.reset();

    std::list<Task> tasks;
    UniqueLock lk{_mutex};

    auto cancelTasks = [&]() {
        tasks.swap(_tasks);
        lk.unlock();
        // Cancel remaining tasks with a CallbackCanceled status.
        for (auto task : tasks) {
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

    Task task = _tasks.front();
    _tasks.pop_front();
    return task;
}

Status TaskRunner::runSynchronousTask(SynchronousTask func, TaskRunner::NextAction nextAction) {
    // Setup cond_var for signaling when done.
    bool done = false;
    stdx::mutex mutex;
    stdx::condition_variable waitTillDoneCond;

    Status returnStatus{Status::OK()};
    this->schedule([&](OperationContext* txn, const Status taskStatus) {
        if (!taskStatus.isOK()) {
            returnStatus = taskStatus;
        } else {
            // Run supplied function.
            try {
                log() << "starting to run synchronous task on runner.";
                returnStatus = func(txn);
                log() << "done running the synchronous task.";
            } catch (...) {
                returnStatus = exceptionToStatus();
                error() << "Exception thrown in runSynchronousTask: " << returnStatus;
            }
        }

        // Signal done.
        LockGuard lk2{mutex};
        done = true;
        waitTillDoneCond.notify_all();

        // return nextAction based on status from supplied function.
        if (returnStatus.isOK()) {
            return nextAction;
        }
        return TaskRunner::NextAction::kCancel;
    });

    UniqueLock lk{mutex};
    waitTillDoneCond.wait(lk, [&done] { return done; });
    return returnStatus;
}
}  // namespace repl
}  // namespace mongo
