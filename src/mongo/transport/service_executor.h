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

#include <functional>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/bitwise_enum_operators.h"
#include "mongo/transport/transport_mode.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/out_of_line_executor.h"

namespace mongo {
// This needs to be forward declared here because the service_context.h is a circular dependency.
class ServiceContext;

namespace transport {

class Session;

/*
 * This is the interface for all ServiceExecutors.
 */
class ServiceExecutor : public OutOfLineExecutor {
public:
    virtual ~ServiceExecutor() = default;
    using Task = unique_function<void()>;
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
    virtual Status scheduleTask(Task task, ScheduleFlags flags) = 0;

    /*
     * Provides an executor-friendly wrapper for "scheduleTask". Internally, it wraps instance of
     * "OutOfLineExecutor::Task" inside "ServiceExecutor::Task" objects, which are then scheduled
     * for execution on the service executor. May throw if "scheduleTask" returns a non-okay status.
     */
    void schedule(OutOfLineExecutor::Task func) override {
        iassert(scheduleTask([task = std::move(func)]() mutable { task(Status::OK()); },
                             ScheduleFlags::kEmptyFlags));
    }

    /*
     * Awaits the availability of incoming data for the specified session. On success, it will
     * schedule the callback on current executor. Otherwise, it will invoke the callback with a
     * non-okay status on the caller thread.
     */
    virtual void runOnDataAvailable(Session* session,
                                    OutOfLineExecutor::Task onCompletionCallback) = 0;

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
