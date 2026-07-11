// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/utility.h"
#include "mongo/transport/session.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] transport {

/*
 * This is the interface for all ServiceExecutors.
 */
class ServiceExecutor {
public:
    using Task = OutOfLineExecutor::Task;

    class TaskRunner : public OutOfLineExecutor {
    public:
        /**
         * Runs the task associated with a session. On success, it will schedule the callback on
         * current executor. Otherwise, it will invoke the callback with a non-okay status on the
         * caller thread.
         */
        virtual void runTaskForSession(std::shared_ptr<Session> session, Task task) = 0;
    };

    /**
     * Starts up all executors registered as ServiceContext decorations.
     * If an executor fails to start up, it will throw and that exception will bubble up here.
     */
    static void startupAll(ServiceContext* svcCtx);

    /**
     * Shuts down all executors registered as ServiceContext decorations.
     * If an executor fails to shut down, a warning will be logged, but shutdowns will continue.
     */
    static void shutdownAll(ServiceContext* svcCtx, Milliseconds timeout);

    /**
     * Append statistics to the `network.serviceExecutors` serverStatus output.
     */
    static void appendAllServerStats(BSONObjBuilder*, ServiceContext*);

    virtual ~ServiceExecutor() = default;

    virtual std::unique_ptr<TaskRunner> makeTaskRunner() = 0;

    /*
     * Starts the ServiceExecutor. This may create threads even if no tasks are scheduled.
     */
    virtual void start() = 0;

    /*
     * Stops and joins the ServiceExecutor. Any outstanding tasks will not be executed, and any
     * associated callbacks waiting on I/O may get called with an error code.
     *
     * This should only be called during server shutdown to gracefully destroy the ServiceExecutor
     */
    virtual Status shutdown(Milliseconds timeout) = 0;

    virtual size_t getRunningThreads() const = 0;

    /** Appends statistics about task scheduling to a BSONObjBuilder for serverStatus output. */
    virtual void appendStats(BSONObjBuilder* bob) const = 0;

    /** Yield if this executor controls more threads than we have cores. */
    void yieldIfAppropriate() const;

    /**
     * Returns the class name of this service executor.
     * Used in logging and exception messaging.
     */
    virtual std::string_view getName() const = 0;
};

/**
 * ServiceExecutorContext determines which ServiceExecutor is used for each Client.
 */
class ServiceExecutorContext {
public:
    // Roughly a 1:1 mapping to the ServiceExecutor type which will be used.
    // ThreadModel::kSynchronous + canUseReserved may result in ServiceExecutorReserved.
    enum class ThreadModel {
        kSynchronous,
        kInline,
    };

    // Manually hoist these enum values into the class to aid callsite usage.
    // As our toolchain is updated, we may be able to replace this with a simple:
    // `using enum ThreadModel;`
    static constexpr inline auto kSynchronous = ThreadModel::kSynchronous;
    static constexpr inline auto kInline = ThreadModel::kInline;

    /**
     * Get a pointer to the ServiceExecutorContext for a given client.
     *
     * This function is valid to invoke either on the Client thread or with the Client lock.
     */
    static ServiceExecutorContext* get(Client* client);

    /**
     * Set the ServiceExecutorContext for a given client.
     *
     * This function may only be invoked once and only while under the Client lock.
     */
    static void set(Client* client, std::unique_ptr<ServiceExecutorContext> seCtx);


    /**
     * Reset the ServiceExecutorContext for a given client.
     *
     * This function may only be invoked once and only while under the Client lock.
     */
    static void reset(Client* client);

    ServiceExecutorContext() = default;
    /** Test only */
    explicit ServiceExecutorContext(std::function<ServiceExecutor*()> getServiceExecutorForTest)
        : _getServiceExecutorForTest(getServiceExecutorForTest) {}
    ServiceExecutorContext(const ServiceExecutorContext&) = delete;
    ServiceExecutorContext& operator=(const ServiceExecutorContext&) = delete;
    ServiceExecutorContext(ServiceExecutorContext&&) = delete;
    ServiceExecutorContext& operator=(ServiceExecutorContext&&) = delete;

    /**
     * Set the thread model for the associated Client's service execution.
     *
     * These functions are only valid to invoke with the Client lock or before the Client is set.
     */
    void setThreadModel(ThreadModel model);

    /**
     * Set if reserved resources are available for the associated Client's service execution.
     *
     * This function is only valid to invoke with the Client lock or before the Client is set.
     */
    void setCanUseReserved(bool canUseReserved);

    /**
     * Get an appropriate ServiceExecutor given the current parameters.
     *
     * This function is only valid to invoke from the associated Client thread. This function does
     * not require the Client lock since all writes must also happen from that thread.
     */
    ServiceExecutor* getServiceExecutor();

private:
    Client* _client = nullptr;

    bool _canUseReserved = false;
    bool _hasUsedSynchronous = false;
    ThreadModel _threadModel{ThreadModel::kSynchronous};

    /** For tests to override the behavior of `getServiceExecutor()`. */
    std::function<ServiceExecutor*()> _getServiceExecutorForTest;
};

/**
 * A small statlet for tracking which executors may be in use.
 */
class ServiceExecutorStats {
public:
    // The number of Clients that are allowed to ignore maxConns and use reserved resources.
    Atomic<std::size_t> limitExempt{0};
};

}  // namespace transport
}  // namespace mongo
