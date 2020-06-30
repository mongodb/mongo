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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "boost/optional.hpp"
#include <algorithm>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_executor_fixed.h"
#include "mongo/transport/service_executor_gen.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/scopeguard.h"

#include <asio.hpp>

namespace mongo {
namespace {
using namespace transport;

namespace {
constexpr Milliseconds kWorkerThreadRunTime{1000};
// Run time + generous scheduling time slice
const Milliseconds kShutdownTime = kWorkerThreadRunTime + Milliseconds{50};
}  // namespace

/* This implements the portions of the transport::Reactor based on ASIO, but leaves out
 * the methods not needed by ServiceExecutors.
 *
 * TODO Maybe use TransportLayerASIO's Reactor?
 */
class ASIOReactor : public transport::Reactor {
public:
    ASIOReactor() : _ioContext() {}

    void run() noexcept final {
        MONGO_UNREACHABLE;
    }

    void runFor(Milliseconds time) noexcept final {
        asio::io_context::work work(_ioContext);

        try {
            _ioContext.run_for(time.toSystemDuration());
        } catch (...) {
            LOGV2_FATAL(50476,
                        "Uncaught exception in reactor: {error}",
                        "Uncaught exception in reactor",
                        "error"_attr = exceptionToStatus());
        }
    }

    void stop() final {
        _ioContext.stop();
    }

    void drain() override final {
        _ioContext.restart();
        while (_ioContext.poll()) {
            LOGV2_DEBUG(22984, 1, "Draining remaining work in reactor.");
        }
        _ioContext.stop();
    }

    std::unique_ptr<ReactorTimer> makeTimer() final {
        MONGO_UNREACHABLE;
    }

    Date_t now() final {
        MONGO_UNREACHABLE;
    }

    void schedule(Task task) final {
        asio::post(_ioContext, [task = std::move(task)] { task(Status::OK()); });
    }

    void dispatch(Task task) final {
        asio::dispatch(_ioContext, [task = std::move(task)] { task(Status::OK()); });
    }

    bool onReactorThread() const final {
        return false;
    }

    operator asio::io_context&() {
        return _ioContext;
    }

private:
    asio::io_context _ioContext;
};

class ServiceExecutorSynchronousFixture : public unittest::Test {
protected:
    void setUp() override {
        auto scOwned = ServiceContext::make();
        setGlobalServiceContext(std::move(scOwned));

        executor = std::make_unique<ServiceExecutorSynchronous>(getGlobalServiceContext());
    }

    std::unique_ptr<ServiceExecutorSynchronous> executor;
};

void scheduleBasicTask(ServiceExecutor* exec, bool expectSuccess) {
    // The barrier is only used when "expectSuccess" is set to true. The ownership of the barrier
    // must be shared between the scheduler and the executor threads to prevent read-after-delete
    // race conditions.
    auto barrier = std::make_shared<unittest::Barrier>(2);
    auto task = [barrier] { barrier->countDownAndWait(); };

    auto status = exec->schedule(std::move(task), ServiceExecutor::kEmptyFlags);
    if (expectSuccess) {
        ASSERT_OK(status);
        barrier->countDownAndWait();
    } else {
        ASSERT_NOT_OK(status);
    }
}

TEST_F(ServiceExecutorSynchronousFixture, BasicTaskRuns) {
    ASSERT_OK(executor->start());
    auto guard = makeGuard([this] { ASSERT_OK(executor->shutdown(kShutdownTime)); });

    scheduleBasicTask(executor.get(), true);
}

TEST_F(ServiceExecutorSynchronousFixture, ScheduleFailsBeforeStartup) {
    scheduleBasicTask(executor.get(), false);
}

class ServiceExecutorFixedFixture : public unittest::Test {
public:
    static constexpr auto kNumExecutorThreads = 2;

    void setUp() override {
        ThreadPool::Options options;
        options.minThreads = options.maxThreads = kNumExecutorThreads;
        options.poolName = "Test";
        _executor = std::make_shared<ServiceExecutorFixed>(std::move(options));
    }

    void tearDown() override {
        if (_skipShutdown)
            return;
        ASSERT_OK(_executor->shutdown(kShutdownTime));
    }

    void skipShutdown(bool skip) {
        _skipShutdown = skip;
    }

    auto getServiceExecutor() const {
        return _executor;
    }

    auto startAndGetServiceExecutor() {
        ASSERT_OK(_executor->start());
        return getServiceExecutor();
    }

    auto makeRecursionGuard() {
        _recursionDepth.fetchAndAdd(1);
        return makeGuard([this] { _recursionDepth.fetchAndSubtract(1); });
    }

    auto getRecursionDepth() const {
        return _recursionDepth.load();
    }

private:
    AtomicWord<int> _recursionDepth{0};
    bool _skipShutdown = false;
    std::shared_ptr<ServiceExecutorFixed> _executor;
};

TEST_F(ServiceExecutorFixedFixture, ScheduleFailsBeforeStartup) {
    auto executor = getServiceExecutor();
    ASSERT_NOT_OK(executor->schedule([] {}, ServiceExecutor::kEmptyFlags));
}

DEATH_TEST_F(ServiceExecutorFixedFixture, DestructorFailsBeforeShutdown, "invariant") {
    startAndGetServiceExecutor();
    skipShutdown(true);
}

TEST_F(ServiceExecutorFixedFixture, BasicTaskRuns) {
    auto executor = startAndGetServiceExecutor();
    auto barrier = std::make_shared<unittest::Barrier>(2);
    ASSERT_OK(executor->schedule([barrier]() mutable { barrier->countDownAndWait(); },
                                 ServiceExecutor::kEmptyFlags));
    barrier->countDownAndWait();
}

TEST_F(ServiceExecutorFixedFixture, RecursiveTask) {
    auto executor = startAndGetServiceExecutor();
    auto barrier = std::make_shared<unittest::Barrier>(2);

    ServiceExecutor::Task recursiveTask;
    recursiveTask = [this, executor, barrier, &recursiveTask] {
        auto recursionGuard = makeRecursionGuard();
        if (getRecursionDepth() < fixedServiceExecutorRecursionLimit.load()) {
            ASSERT_OK(executor->schedule(recursiveTask, ServiceExecutor::kMayRecurse));
        } else {
            // This test never returns unless the service executor can satisfy the recursion depth.
            barrier->countDownAndWait();
        }
    };

    // Schedule recursive task and wait for the recursion to stop
    ASSERT_OK(executor->schedule(recursiveTask, ServiceExecutor::kMayRecurse));
    barrier->countDownAndWait();
}

TEST_F(ServiceExecutorFixedFixture, FlattenRecursiveScheduledTasks) {
    auto executor = startAndGetServiceExecutor();
    auto barrier = std::make_shared<unittest::Barrier>(2);
    AtomicWord<int> tasksToSchedule{fixedServiceExecutorRecursionLimit.load() * 3};

    // A recursive task that expects to be executed non-recursively. The task recursively schedules
    // "tasksToSchedule" tasks to the service executor, and each scheduled task verifies that the
    // recursion depth remains zero during its execution.
    ServiceExecutor::Task recursiveTask;
    recursiveTask = [this, executor, barrier, &recursiveTask, &tasksToSchedule] {
        auto recursionGuard = makeRecursionGuard();
        ASSERT_EQ(getRecursionDepth(), 1);
        if (tasksToSchedule.fetchAndSubtract(1) > 0) {
            ASSERT_OK(executor->schedule(recursiveTask, ServiceExecutor::kEmptyFlags));
        } else {
            // Once there are no more tasks to schedule, notify the main thread to proceed.
            barrier->countDownAndWait();
        }
    };

    // Schedule the recursive task and wait for the execution to finish.
    ASSERT_OK(executor->schedule(recursiveTask, ServiceExecutor::kMayYieldBeforeSchedule));
    barrier->countDownAndWait();
}

TEST_F(ServiceExecutorFixedFixture, ShutdownTimeLimit) {
    auto executor = startAndGetServiceExecutor();
    auto invoked = std::make_shared<SharedPromise<void>>();
    auto mayReturn = std::make_shared<SharedPromise<void>>();

    ASSERT_OK(executor->schedule(
        [executor, invoked, mayReturn]() mutable {
            invoked->emplaceValue();
            mayReturn->getFuture().get();
        },
        ServiceExecutor::kEmptyFlags));

    invoked->getFuture().get();
    ASSERT_NOT_OK(executor->shutdown(kShutdownTime));

    // Ensure the service executor is stopped before leaving the test.
    mayReturn->emplaceValue();
}

TEST_F(ServiceExecutorFixedFixture, Stats) {
    auto executor = startAndGetServiceExecutor();
    auto barrier = std::make_shared<unittest::Barrier>(kNumExecutorThreads + 1);
    stdx::condition_variable cond;

    auto task = [barrier, &cond]() mutable {
        cond.notify_one();
        barrier->countDownAndWait();
    };

    for (auto i = 0; i < kNumExecutorThreads; i++) {
        ASSERT_OK(executor->schedule(task, ServiceExecutor::kEmptyFlags));
    }

    // The main thread waits for the executor threads to bump up "threadsRunning" while picking up a
    // task to execute. Once all executor threads are running, the main thread will unblock them
    // through the barrier.
    auto mutex = MONGO_MAKE_LATCH();
    stdx::unique_lock<Latch> lk(mutex);
    cond.wait(lk, [&executor]() {
        BSONObjBuilder bob;
        executor->appendStats(&bob);
        auto obj = bob.obj();
        ASSERT(obj.hasField("threadsRunning"));
        auto threadsRunning = obj.getIntField("threadsRunning");
        LOGV2_DEBUG(
            4910503, 1, "Checked number of executor threads", "threads"_attr = threadsRunning);
        return threadsRunning == static_cast<int>(ServiceExecutorFixedFixture::kNumExecutorThreads);
    });

    barrier->countDownAndWait();
}

TEST_F(ServiceExecutorFixedFixture, ScheduleFailsAfterShutdown) {
    auto executor = startAndGetServiceExecutor();
    std::unique_ptr<stdx::thread> schedulerThread;

    {
        // Spawn a thread to schedule a task, and block it before it can schedule the task with the
        // underlying thread-pool. Then shutdown the service executor and unblock the scheduler
        // thread. This order of events must cause "schedule()" to return a non-okay status.
        FailPointEnableBlock failpoint("hangBeforeSchedulingServiceExecutorFixedTask");
        schedulerThread = std::make_unique<stdx::thread>([executor] {
            ASSERT_NOT_OK(
                executor->schedule([] { MONGO_UNREACHABLE; }, ServiceExecutor::kEmptyFlags));
        });
        failpoint->waitForTimesEntered(1);
        ASSERT_OK(executor->shutdown(kShutdownTime));
    }

    schedulerThread->join();
}

}  // namespace
}  // namespace mongo
