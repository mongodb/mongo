// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class OperationContext;

namespace executor {

/**
 * Implements a scoped task executor which collects all callback handles it receives as part of
 * running operations and cancels any outstanding ones on destruction.
 *
 * The intent is that you can use this type with arbitrary task executor taking functions and allow
 * this object's destruction to clean up any methods which returned callback handles.
 *
 * Note that while this type provides access to a TaskExecutor*, certain methods are illegal to
 * call:
 * - startup()
 * - appendDiagnosticBSON()
 * - appendConnectionStats()
 * - dropConnections()
 *
 * And certain other methods only pass through this class to the underlying executor:
 * - makeEvent()
 *     will return a not-ok status after shutdown, but does not otherwise instrument the event
 * - signalEvent()
 *     always signals
 * - waitForEvent()
 *     always waits for the event
 * - cancel()
 *     always cancels the task
 * - wait()
 *     always waits for the task
 * - now()
 *     always returns the time
 *
 * This leaves the various callback handle returning methods + join/shutdown.  These methods, rather
 * than performing a passthrough, address only tasks dispatched through this executor, rather than
 * passing through to the underlying executor.
 *
 * Also note that this type DOES NOT call join in its dtor.  I.e. all tasks are cancelled, but all
 * callbacks may not have run when this object goes away.  You must call join() if you want that
 * guarantee.
 */
class ScopedTaskExecutor {
public:
    explicit ScopedTaskExecutor(std::shared_ptr<TaskExecutor> executor);
    // Tasks respond with the supplied status on shutdown. Status must be in the CancellationError
    // category.
    ScopedTaskExecutor(std::shared_ptr<TaskExecutor> executor, Status shutdownError);

    // Delete all move/copy-ability
    ScopedTaskExecutor(ScopedTaskExecutor&&) = delete;

    ~ScopedTaskExecutor();

    operator TaskExecutor*() const {
        return _executor.get();
    }

    const std::shared_ptr<TaskExecutor>& operator*() const {
        return _executor;
    }

    TaskExecutor* operator->() const {
        return _executor.get();
    }

private:
    class Impl;

    std::shared_ptr<TaskExecutor> _executor;
};

[[MONGO_MOD_FILE_PRIVATE]] extern FailPoint ScopedTaskExecutorHangBeforeSchedule;
[[MONGO_MOD_FILE_PRIVATE]] extern FailPoint ScopedTaskExecutorHangExitBeforeSchedule;
[[MONGO_MOD_FILE_PRIVATE]] extern FailPoint ScopedTaskExecutorHangAfterSchedule;

}  // namespace executor
}  // namespace mongo
