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

#include <boost/thread/locks.hpp>
#include <memory>

#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

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
        }
        catch (...) {
            log() << "Unhandled exception in task runner: " << exceptionToStatus();
        }
        return TaskRunner::NextAction::kCancel;
    }

} // namespace

    // static
    TaskRunner::Task TaskRunner::makeCancelTask() {
        return [](OperationContext* txn, const Status& status) {
            return NextAction::kCancel;
        };
    }

    TaskRunner::TaskRunner(threadpool::ThreadPool* threadPool,
                           const CreateOperationContextFn& createOperationContext)
        : _threadPool(threadPool),
          _createOperationContext(createOperationContext),
          _active(false),
          _cancelRequested(false) {

        uassert(ErrorCodes::BadValue, "null thread pool", threadPool);
        uassert(ErrorCodes::BadValue, "null operation context factory", createOperationContext);
    }

    TaskRunner::~TaskRunner() {
        try {
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            if (!_active) {
                return;
            }
            _cancelRequested = true;
            _condition.notify_all();
            while (_active) {
                _condition.wait(lk);
            }
        }
        catch (...) {
            error() << "unexpected exception destroying task runner: " << exceptionToStatus();
        }
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

    void TaskRunner::_runTasks() {
        std::unique_ptr<OperationContext> txn;

        while (Task task = _waitForNextTask()) {
            if (!txn) {
                txn.reset(_createOperationContext());
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
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            tasks.swap(_tasks);
        }

        // Cancel remaining tasks with a CallbackCanceled status.
        for (auto task : tasks) {
            runSingleTask(task, nullptr, Status(ErrorCodes::CallbackCanceled,
                "this task has been canceled by a previously invoked task"));
        }

        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _finishRunTasks_inlock();
    }

    void TaskRunner::_finishRunTasks_inlock() {
        _active = false;
        _cancelRequested = false;
        _condition.notify_all();
    }

    TaskRunner::Task TaskRunner::_waitForNextTask() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

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

} // namespace repl
} // namespace mongo
