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


#include "mongo/platform/basic.h"

#include "boost/optional.hpp"
#include <algorithm>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/service_executor_fixed.h"
#include "mongo/transport/service_executor_gen.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/scopeguard.h"

#include <asio.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::transport {
namespace {

constexpr auto kWorkerThreadRunTime = Milliseconds{1000};
// Run time + generous scheduling time slice
constexpr auto kShutdownTime = Milliseconds{kWorkerThreadRunTime.count() + 50};

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

class ServiceExecutorSynchronousTest : public unittest::Test {
public:
    void setUp() override {
        setGlobalServiceContext(ServiceContext::make());
    }

    void tearDown() override {
        setGlobalServiceContext(nullptr);
    }
};

TEST_F(ServiceExecutorSynchronousTest, BasicTaskRuns) {
    unittest::Barrier barrier(2);
    ServiceExecutorSynchronous executor(getGlobalServiceContext());
    ASSERT_OK(executor.start());
    ASSERT_OK(executor.scheduleTask([&] { barrier.countDownAndWait(); }, {}));
    barrier.countDownAndWait();
    ASSERT_OK(executor.shutdown(kShutdownTime));
}

TEST_F(ServiceExecutorSynchronousTest, ScheduleFailsBeforeStartup) {
    ServiceExecutorSynchronous executor(getGlobalServiceContext());
    ASSERT_NOT_OK(executor.scheduleTask([] {}, {}));
}

class ServiceExecutorFixedTest : public unittest::Test {
public:
    static constexpr size_t kExecutorThreads = 2;

    class Handle {
    public:
        Handle() = default;
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        ~Handle() {
            join();
        }

        void join() {
            ASSERT_OK(_executor->shutdown(kShutdownTime));
        }

        void start() {
            ASSERT_OK(_executor->start());
        }

        ServiceExecutorFixed* operator->() const noexcept {
            return &*_executor;
        }

        ServiceExecutorFixed& operator*() const noexcept {
            return *_executor;
        }

    private:
        std::shared_ptr<ServiceExecutorFixed> _executor{std::make_shared<ServiceExecutorFixed>(
            ThreadPool::Limits{kExecutorThreads, kExecutorThreads})};
    };
};

TEST_F(ServiceExecutorFixedTest, ScheduleFailsBeforeStartup) {
    Handle handle;
    ASSERT_NOT_OK(handle->scheduleTask([] {}, {}));
}

TEST_F(ServiceExecutorFixedTest, BasicTaskRuns) {
    unittest::Barrier barrier(2);
    Handle handle;
    handle.start();

    ASSERT_OK(handle->scheduleTask([&] { barrier.countDownAndWait(); }, {}));
    barrier.countDownAndWait();
}

TEST_F(ServiceExecutorFixedTest, RecursiveTask) {
    unittest::Barrier barrier(2);
    Handle handle;
    handle.start();

    std::function<void()> recursiveTask = [&] {
        if (handle->getRecursionDepthForExecutorThread() <
            fixedServiceExecutorRecursionLimit.load()) {
            ASSERT_OK(
                handle->scheduleTask(recursiveTask, ServiceExecutor::ScheduleFlags::kMayRecurse));
        } else {
            // This test never returns unless the service executor can satisfy the recursion depth.
            barrier.countDownAndWait();
        }
    };

    // Schedule recursive task and wait for the recursion to stop
    ASSERT_OK(handle->scheduleTask(recursiveTask, ServiceExecutor::ScheduleFlags::kMayRecurse));
    barrier.countDownAndWait();
}

TEST_F(ServiceExecutorFixedTest, FlattenRecursiveScheduledTasks) {
    unittest::Barrier barrier(2);
    Handle handle;
    handle.start();
    AtomicWord<int> tasksToSchedule{fixedServiceExecutorRecursionLimit.load() * 3};

    // A recursive task that expects to be executed non-recursively. The task recursively schedules
    // "tasksToSchedule" tasks to the service executor, and each scheduled task verifies that the
    // recursion depth remains zero during its execution.
    std::function<void()> recursiveTask = [&] {
        ASSERT_EQ(handle->getRecursionDepthForExecutorThread(), 1);
        if (tasksToSchedule.fetchAndSubtract(1) > 0) {
            ASSERT_OK(handle->scheduleTask(recursiveTask, {}));
        } else {
            // Once there are no more tasks to schedule, notify the main thread to proceed.
            barrier.countDownAndWait();
        }
    };

    // Schedule the recursive task and wait for the execution to finish.
    ASSERT_OK(handle->scheduleTask(recursiveTask,
                                   ServiceExecutor::ScheduleFlags::kMayYieldBeforeSchedule));
    barrier.countDownAndWait();
}

TEST_F(ServiceExecutorFixedTest, ShutdownTimeLimit) {
    SharedPromise<void> invoked;
    SharedPromise<void> mayReturn;

    Handle handle;
    handle.start();

    ASSERT_OK(handle->scheduleTask(
        [&] {
            invoked.emplaceValue();
            mayReturn.getFuture().get();
        },
        {}));

    invoked.getFuture().get();
    ASSERT_NOT_OK(handle->shutdown(kShutdownTime));

    // Ensure the service executor is stopped before leaving the test.
    mayReturn.emplaceValue();
}

TEST_F(ServiceExecutorFixedTest, ScheduleSucceedsBeforeShutdown) {
    unittest::threadAssertionMonitoredTest([&](auto&& monitor) {
        unittest::Barrier barrier(2);
        Handle handle;
        handle.start();

        stdx::thread scheduleClient;
        ScopeGuard joinGuard([&] { scheduleClient.join(); });

        {
            FailPointEnableBlock failpoint("hangBeforeSchedulingServiceExecutorFixedTask");

            // The executor accepts the work, but hasn't used the underlying pool yet.
            scheduleClient = monitor.spawn(
                [&] { ASSERT_OK(handle->scheduleTask([&] { barrier.countDownAndWait(); }, {})); });
            failpoint->waitForTimesEntered(1);

            // Trigger an immediate shutdown which will not affect the task we have accepted.
            ASSERT_NOT_OK(handle->shutdown(Milliseconds{0}));
        }

        // Our failpoint has been disabled, so the task can run to completion.
        barrier.countDownAndWait();

        // Now we can wait for the task to finish and shutdown.
        ASSERT_OK(handle->shutdown(kShutdownTime));
    });
}

TEST_F(ServiceExecutorFixedTest, ScheduleFailsAfterShutdown) {
    Handle handle;
    handle.start();

    ASSERT_OK(handle->shutdown(kShutdownTime));
    ASSERT_NOT_OK(handle->scheduleTask([] { MONGO_UNREACHABLE; }, {}));
}

TEST_F(ServiceExecutorFixedTest, RunTaskAfterWaitingForData) {
    unittest::threadAssertionMonitoredTest([&](auto&& monitor) {
        unittest::Barrier barrier(2);
        auto tl = std::make_unique<TransportLayerMock>();
        auto session = std::dynamic_pointer_cast<MockSession>(tl->createSession());
        invariant(session);

        Handle handle;
        handle.start();

        const auto signallingThreadId = stdx::this_thread::get_id();

        AtomicWord<bool> ranOnDataAvailable{false};

        handle->runOnDataAvailable(session, [&](Status) {
            ranOnDataAvailable.store(true);
            ASSERT(stdx::this_thread::get_id() != signallingThreadId);
            barrier.countDownAndWait();
        });

        ASSERT(!ranOnDataAvailable.load());

        session->signalAvailableData();

        barrier.countDownAndWait();
        ASSERT(ranOnDataAvailable.load());
    });
}

TEST_F(ServiceExecutorFixedTest, StartAndShutdownAreDeterministic) {
    unittest::threadAssertionMonitoredTest([&](auto&& monitor) {
        Handle handle;

        // Ensure starting the executor results in spawning the specified number of executor
        // threads.
        {
            FailPointEnableBlock failpoint("hangAfterServiceExecutorFixedExecutorThreadsStart");
            handle.start();
            failpoint->waitForTimesEntered(kExecutorThreads);
        }

        // Since destroying ServiceExecutorFixed is blocking, spawn a thread to issue the
        // destruction off of the main execution path.
        stdx::thread shutdownThread;

        // Ensure all executor threads return after receiving the shutdown signal.
        {
            FailPointEnableBlock failpoint(
                "hangBeforeServiceExecutorFixedLastExecutorThreadReturns");
            shutdownThread = monitor.spawn([&] { handle.join(); });
            failpoint->waitForTimesEntered(1);
        }
        shutdownThread.join();
    });
}

}  // namespace
}  // namespace mongo::transport
