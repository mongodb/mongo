/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/condition_variable.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/fail_point.h"

namespace mongo {

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

    // Delete all move/copy-ability
    ScopedTaskExecutor(TaskExecutor&&) = delete;

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

extern FailPoint ScopedTaskExecutorHangBeforeSchedule;
extern FailPoint ScopedTaskExecutorHangExitBeforeSchedule;
extern FailPoint ScopedTaskExecutorHangAfterSchedule;

}  // namespace executor
}  // namespace mongo
