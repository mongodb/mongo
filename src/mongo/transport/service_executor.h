/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/bitwise_enum_operators.h"
#include "mongo/stdx/functional.h"
#include "mongo/transport/service_executor_task_names.h"
#include "mongo/transport/transport_mode.h"
#include "mongo/util/duration.h"

namespace mongo {
// This needs to be forward declared here because the service_context.h is a circular dependency.
class ServiceContext;

namespace transport {

/*
 * This is the interface for all ServiceExecutors.
 */
class ServiceExecutor {
public:
    virtual ~ServiceExecutor() = default;
    using Task = stdx::function<void()>;
    enum ScheduleFlags {
        // No flags (kEmptyFlags) specifies that this is a normal task and that the executor should
        // launch new threads as needed to run the task.
        kEmptyFlags = 1 << 0,

        // Deferred tasks will never get a new thread launched to run them.
        kDeferredTask = 1 << 1,

        // MayRecurse indicates that a task may be run recursively.
        kMayRecurse = 1 << 2,

        // MayYieldBeforeSchedule indicates that the executor may yield on the current thread before
        // scheduling the task.
        kMayYieldBeforeSchedule = 1 << 3,
    };

    /*
     * Starts the ServiceExecutor. This may create threads even if no tasks are scheduled.
     */
    virtual Status start() = 0;

    /*
     * Schedules a task with the ServiceExecutor and returns immediately.
     *
     * This is guaranteed to unwind the stack before running the task, although the task may be
     * run later in the same thread.
     *
     * If defer is true, then the executor may defer execution of this Task until an available
     * thread is available.
     */
    virtual Status schedule(Task task, ScheduleFlags flags, ServiceExecutorTaskName taskName) = 0;

    /*
     * Stops and joins the ServiceExecutor. Any outstanding tasks will not be executed, and any
     * associated callbacks waiting on I/O may get called with an error code.
     *
     * This should only be called during server shutdown to gracefully destroy the ServiceExecutor
     */
    virtual Status shutdown(Milliseconds timeout) = 0;

    /*
     * Returns if this service executor is using asynchronous or synchronous networking.
     */
    virtual Mode transportMode() const = 0;

    /*
     * Appends statistics about task scheduling to a BSONObjBuilder for serverStatus output.
     */
    virtual void appendStats(BSONObjBuilder* bob) const = 0;
};

}  // namespace transport

ENABLE_BITMASK_OPERATORS(transport::ServiceExecutor::ScheduleFlags)

}  // namespace mongo
