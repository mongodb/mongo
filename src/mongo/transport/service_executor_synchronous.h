// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/util/duration.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <string_view>

namespace mongo::transport {
/** Transitional for differential benchmarking of ServiceExecutorSynchronous refactor */
#define TRANSITIONAL_SERVICE_EXECUTOR_SYNCHRONOUS_HAS_RESERVE 0

namespace service_executor_synchronous_detail {
using namespace std::literals::string_view_literals;

/**
 * Provides common functionality for ServiceExecutorSynchronous and ServiceExecutorInline.
 */
class ServiceExecutorSyncImpl : public ServiceExecutor {
public:
    ~ServiceExecutorSyncImpl() override;

    void start() override;
    Status shutdown(Milliseconds timeout) override;

    /**
     * The first time a Task is scheduled on the returned TaskRunner,
     * a queue is created such that the first Task and any subsequent Tasks
     * scheduled into it are executed in order.
     */
    std::unique_ptr<TaskRunner> makeTaskRunner() override;

    size_t getRunningThreads() const override;

    void appendStats(BSONObjBuilder* bob) const override;

    std::string_view getName() const override {
        return "ServiceExecutorSynchronous"sv;
    }

protected:
    enum class RunTaskInline : bool {};
    explicit ServiceExecutorSyncImpl(RunTaskInline runTaskInline, std::string statsFieldName);

private:
    class SharedState;
    std::shared_ptr<SharedState> _sharedState;

    const std::string _statsFieldName;
};

}  // namespace service_executor_synchronous_detail

/**
 * Creates a fresh worker thread for each top-level scheduled task. Any tasks
 * scheduled during the execution of that top-level task as it runs on such a
 * worker thread are pushed to the queue of that worker thread.
 *
 * Thus, the top-level task is expected to represent a chain of operations, each
 * of which schedules its successor before returning. The entire chain of
 * operations, and nothing else, executes on the same worker thread.
 */
class ServiceExecutorSynchronous
    : public service_executor_synchronous_detail::ServiceExecutorSyncImpl {
public:
    ServiceExecutorSynchronous() : ServiceExecutorSyncImpl(RunTaskInline{false}, "passthrough") {}
    static ServiceExecutorSynchronous* get(ServiceContext* ctx);
};

/**
 * Behaves similarly to ServiceExecutorSynchronous above,
 * however the task queue is run on the same thread as called the scheduler initially.
 * Thus, unlike ServiceExecutorSynchronous, the initial call to TaskRunner::schedule()
 * will appear to block on the entire queue of Tasks until completion.
 */
class ServiceExecutorInline : public service_executor_synchronous_detail::ServiceExecutorSyncImpl {
public:
    ServiceExecutorInline() : ServiceExecutorSyncImpl(RunTaskInline{true}, "inline") {}
    static ServiceExecutorInline* get(ServiceContext*);
};

}  // namespace mongo::transport
