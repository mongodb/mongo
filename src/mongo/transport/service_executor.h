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
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/utility.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/out_of_line_executor.h"

namespace mongo {
namespace transport {

extern bool gInitialUseDedicatedThread;

/*
 * This is the interface for all ServiceExecutors.
 */
class ServiceExecutor : public OutOfLineExecutor {
public:
    static void shutdownAll(ServiceContext* serviceContext, Date_t deadline);

    virtual ~ServiceExecutor() = default;
    using Task = unique_function<void()>;

    /** With no flags set, `scheduleTask` will launch new threads as needed. */
    enum class ScheduleFlags {
        kDeferredTask = 1 << 0,           /**< Never given a newly launched thread. */
        kMayRecurse = 1 << 1,             /**< May be run recursively. */
        kMayYieldBeforeSchedule = 1 << 2, /**< May yield before scheduling. */
    };
    friend constexpr ScheduleFlags operator&(ScheduleFlags a, ScheduleFlags b) noexcept {
        return ScheduleFlags{stdx::to_underlying(a) & stdx::to_underlying(b)};
    }
    friend constexpr ScheduleFlags operator|(ScheduleFlags a, ScheduleFlags b) noexcept {
        return ScheduleFlags{stdx::to_underlying(a) | stdx::to_underlying(b)};
    }

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
        iassert(scheduleTask([task = std::move(func)]() mutable { task(Status::OK()); }, {}));
    }

    /*
     * Awaits the availability of incoming data for the specified session. On success, it will
     * schedule the callback on current executor. Otherwise, it will invoke the callback with a
     * non-okay status on the caller thread.
     */
    virtual void runOnDataAvailable(const SessionHandle& session,
                                    OutOfLineExecutor::Task onCompletionCallback) = 0;

    /*
     * Stops and joins the ServiceExecutor. Any outstanding tasks will not be executed, and any
     * associated callbacks waiting on I/O may get called with an error code.
     *
     * This should only be called during server shutdown to gracefully destroy the ServiceExecutor
     */
    virtual Status shutdown(Milliseconds timeout) = 0;

    virtual size_t getRunningThreads() const = 0;

    /*
     * Appends statistics about task scheduling to a BSONObjBuilder for serverStatus output.
     */
    virtual void appendStats(BSONObjBuilder* bob) const = 0;

    /**
     * Yield if we have more threads than cores.
     */
    void yieldIfAppropriate() const;
};

/**
 * ServiceExecutorContext determines which ServiceExecutor is used for each Client.
 */
class ServiceExecutorContext {
public:
    /**
     * Get a pointer to the ServiceExecutorContext for a given client.
     *
     * This function is valid to invoke either on the Client thread or with the Client lock.
     */
    static ServiceExecutorContext* get(Client* client) noexcept;

    /**
     * Set the ServiceExecutorContext for a given client.
     *
     * This function may only be invoked once and only while under the Client lock.
     */
    static void set(Client* client, std::unique_ptr<ServiceExecutorContext> seCtx) noexcept;


    /**
     * Reset the ServiceExecutorContext for a given client.
     *
     * This function may only be invoked once and only while under the Client lock.
     */
    static void reset(Client* client) noexcept;

    ServiceExecutorContext() = default;
    ServiceExecutorContext(const ServiceExecutorContext&) = delete;
    ServiceExecutorContext& operator=(const ServiceExecutorContext&) = delete;
    ServiceExecutorContext(ServiceExecutorContext&&) = delete;
    ServiceExecutorContext& operator=(ServiceExecutorContext&&) = delete;

    /**
     * Set the threading model for the associated Client's service execution.
     *
     * This function is only valid to invoke with the Client lock or before the Client is set.
     */
    void setUseDedicatedThread(bool dedicated) noexcept;

    /**
     * Set if reserved resources are available for the associated Client's service execution.
     *
     * This function is only valid to invoke with the Client lock or before the Client is set.
     */
    void setCanUseReserved(bool canUseReserved) noexcept;

    /**
     * Get the ThreadingModel for the associated Client.
     *
     * This function is valid to invoke either on the Client thread or with the Client lock.
     */
    bool useDedicatedThread() const noexcept {
        return _useDedicatedThread;
    }

    /**
     * Get an appropriate ServiceExecutor given the current parameters.
     *
     * This function is only valid to invoke from the associated Client thread. This function does
     * not require the Client lock since all writes must also happen from that thread.
     */
    ServiceExecutor* getServiceExecutor() noexcept;

private:
    Client* _client = nullptr;
    ServiceEntryPoint* _sep = nullptr;

    bool _useDedicatedThread = true;
    bool _canUseReserved = false;
    bool _hasUsedSynchronous = false;
};

/**
 * A small statlet for tracking which executors may be in use.
 */
class ServiceExecutorStats {
public:
    /**
     * Get the current value of ServiceExecutorStats for the given ServiceContext.
     *
     * Note that this value is intended for statistics and logging. It is unsynchronized and
     * unsuitable for informing decisions in runtime.
     */
    static ServiceExecutorStats get(ServiceContext* ctx) noexcept;

    // The number of Clients who use the dedicated executors.
    size_t usesDedicated = 0;

    // The number of Clients who use the borrowed executors.
    size_t usesBorrowed = 0;

    // The number of Clients that are allowed to ignore maxConns and use reserved resources.
    size_t limitExempt = 0;
};

}  // namespace transport

}  // namespace mongo
