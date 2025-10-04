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

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <list>
#include <string>

namespace MONGO_MOD_PARENT_PRIVATE mongo {

class Status;
class OperationContext;

namespace repl {

class TaskRunner {
    TaskRunner(const TaskRunner&) = delete;
    TaskRunner& operator=(const TaskRunner&) = delete;

public:
    /**
     * Represents next steps of task runner.
     */
    enum class NextAction {
        kInvalid = 0,
        kDisposeOperationContext = 1,
        kCancel = 3,
    };

    using Task = unique_function<NextAction(OperationContext*, const Status&)>;

    explicit TaskRunner(ThreadPool* threadPool);

    virtual ~TaskRunner();

    /**
     * Returns diagnostic information.
     */
    std::string getDiagnosticString() const;

    /**
     * Returns true if there are any scheduled or actively running tasks.
     */
    bool isActive() const;

    /**
     * Schedules a task to be run by the task runner. Tasks are run in the same order that they
     * are scheduled.
     *
     * This transitions the task runner to an active state.
     *
     * The task runner creates an operation context using the current client
     * prior to running a scheduled task. Depending on the NextAction returned from the
     * task, operation contexts may be shared between consecutive tasks invoked by the task
     * runner.
     *
     * On completion, each task is expected to return a NextAction to the task runner.
     *
     *     If the task returns kDisposeOperationContext, the task runner destroys the operation
     *     context. The next task to be invoked will receive a new operation context.
     *
     *     If the task returns kCancel, the task runner will destroy the operation context and
     *     cancel the remaining tasks (each task will be invoked with a status containing the
     *     code ErrorCodes::CallbackCanceled). After all the tasks have been canceled, the task
     *     runner will become inactive.
     *
     *     If the task returns kInvalid, this NextAction will be handled in the same way as
     *     kDisposeOperationContext.
     *
     * If the status passed to the task is not OK, the task should not proceed and return
     * immediately. This is usually the case when the task runner is canceled. Accessing the
     * operation context in the task will result in undefined behavior.
     */
    void schedule(Task task);

    /**
     * If there is a task that is already running, allows the task to run to completion.
     * Cancels all scheduled tasks that have not been run. Canceled tasks will still be
     * invoked with a status containing the code ErrorCodes::CallbackCanceled.
     * After all active tasks have completed and unscheduled tasks have been canceled, the
     * task runner will go into an inactive state.
     *
     * It is a no-op to call cancel() before scheduling any tasks.
     */
    void cancel();

    /**
     * Waits for the task runner to become inactive.
     */
    void join();

private:
    /**
     * Runs tasks in a loop.
     * Loop exits when any of the tasks returns a non-kContinue next action.
     */
    void _runTasks();
    void _finishRunTasks(WithLock lk);

    /**
     * Waits for next scheduled task to be added to queue.
     * Returns null task when task runner is stopped.
     */
    Task _waitForNextTask();

    ThreadPool* _threadPool;

    // Protects member data of this TaskRunner.
    mutable stdx::mutex _mutex;

    stdx::condition_variable _condition;

    // _active is true when there are scheduled tasks in the task queue or
    // when a task is being run by the task runner.
    bool _active;

    bool _cancelRequested;

    // FIFO queue of scheduled tasks
    std::list<Task> _tasks;
};

}  // namespace repl
}  // namespace MONGO_MOD_PARENT_PRIVATE mongo
