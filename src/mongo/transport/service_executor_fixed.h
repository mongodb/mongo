/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/hierarchical_acquisition.h"

namespace mongo {
namespace transport {

/**
 * A service executor that uses a fixed (configurable) number of threads to execute tasks.
 * This executor always yields before executing scheduled tasks, and never yields before scheduling
 * new tasks (i.e., `ScheduleFlags::kMayYieldBeforeSchedule` is a no-op for this executor).
 */
class ServiceExecutorFixed final : public ServiceExecutor,
                                   public std::enable_shared_from_this<ServiceExecutorFixed> {
    static constexpr auto kDiagnosticLogLevel = 3;

    static const inline auto kInShutdown =
        Status(ErrorCodes::ServiceExecutorInShutdown, "ServiceExecutorFixed is not running");

public:
    explicit ServiceExecutorFixed(ServiceContext* ctx, ThreadPool::Limits limits);
    explicit ServiceExecutorFixed(ThreadPool::Limits limits)
        : ServiceExecutorFixed(nullptr, std::move(limits)) {}
    virtual ~ServiceExecutorFixed();

    static ServiceExecutorFixed* get(ServiceContext* ctx);

    Status start() override;
    Status shutdown(Milliseconds timeout) override;
    Status scheduleTask(Task task, ScheduleFlags flags) override;
    void schedule(OutOfLineExecutor::Task task) override {
        _schedule(std::move(task));
    }

    void runOnDataAvailable(const SessionHandle& session,
                            OutOfLineExecutor::Task onCompletionCallback) override;

    Mode transportMode() const override {
        return Mode::kSynchronous;
    }

    void appendStats(BSONObjBuilder* bob) const override;

    /**
     * Returns the recursion depth of the active executor thread.
     * It is forbidden to invoke this method outside scheduled tasks.
     */
    int getRecursionDepthForExecutorThread() const;

private:
    // Maintains the execution state (e.g., recursion depth) for executor threads
    class ExecutorThreadContext;

private:
    void _checkForShutdown(WithLock);
    void _schedule(OutOfLineExecutor::Task task) noexcept;

    auto _threadsRunning() const {
        return _stats.threadsStarted.load() - _stats.threadsEnded.loadRelaxed();
    }

    auto _tasksRunning() const {
        return _stats.tasksStarted.load() - _stats.tasksEnded.loadRelaxed();
    }

    auto _tasksLeft() const {
        return _stats.tasksScheduled.load() - _stats.tasksEnded.loadRelaxed();
    }

    struct Stats {
        AtomicWord<size_t> threadsStarted{0};
        AtomicWord<size_t> threadsEnded{0};

        AtomicWord<size_t> tasksScheduled{0};
        AtomicWord<size_t> tasksStarted{0};
        AtomicWord<size_t> tasksEnded{0};
    };
    Stats _stats;

    ServiceContext* const _svcCtx;

    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "ServiceExecutorFixed::_mutex");
    stdx::condition_variable _shutdownCondition;
    SharedPromise<void> _shutdownComplete;

    /**
     * State transition diagram: kNotStarted ---> kRunning ---> kStopping ---> kStopped
     */
    enum State { kNotStarted, kRunning, kStopping, kStopped } _state = kNotStarted;

    ThreadPool::Options _options;
    std::shared_ptr<ThreadPool> _threadPool;

    struct Waiter {
        SessionHandle session;
        OutOfLineExecutor::Task onCompletionCallback;
    };
    using WaiterList = std::list<Waiter>;
    WaiterList _waiters;

    static thread_local std::unique_ptr<ExecutorThreadContext> _executorContext;
};

}  // namespace transport
}  // namespace mongo
