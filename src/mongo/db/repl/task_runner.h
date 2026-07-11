// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <list>
#include <mutex>
#include <string>

namespace [[MONGO_MOD_PARENT_PRIVATE]] mongo {

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
    mutable std::mutex _mutex;

    stdx::condition_variable _condition;

    // _active is true when there are scheduled tasks in the task queue or
    // when a task is being run by the task runner.
    bool _active;

    bool _cancelRequested;

    // FIFO queue of scheduled tasks
    std::list<Task> _tasks;
};

}  // namespace repl
}  // namespace mongo
