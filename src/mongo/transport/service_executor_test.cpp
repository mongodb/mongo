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

    auto status = exec->scheduleTask(std::move(task), ServiceExecutor::kEmptyFlags);
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

class ServiceExecutorFixedFixture : public ServiceContextTest {
public:
    static constexpr auto kNumExecutorThreads = 2;

    class ServiceExecutorHandle {
    public:
        static constexpr int kNone = 0b00;
        static constexpr int kStartExecutor = 0b01;
        static constexpr int kSkipShutdown = 0b10;

        ServiceExecutorHandle() = delete;
        ServiceExecutorHandle(const ServiceExecutorHandle&) = delete;
        ServiceExecutorHandle(ServiceExecutorHandle&&) = delete;

        explicit ServiceExecutorHandle(int flags = kNone) : _skipShutdown(flags & kSkipShutdown) {
            ThreadPool::Limits limits;
            limits.minThreads = limits.maxThreads = kNumExecutorThreads;
            _executor = std::make_shared<ServiceExecutorFixed>(std::move(limits));

            if (flags & kStartExecutor) {
                ASSERT_OK(_executor->start());
            }
        }

        ~ServiceExecutorHandle() {
            if (_skipShutdown) {
                LOGV2(4987901, "Skipped shutting down the executor");
            } else {
                invariant(_executor->shutdown(kShutdownTime));
            }
        }

        std::shared_ptr<ServiceExecutorFixed> operator->() noexcept {
            return _executor;
        }

        std::shared_ptr<ServiceExecutorFixed> operator*() noexcept {
            return _executor;
        }

    private:
        const bool _skipShutdown;
        std::shared_ptr<ServiceExecutorFixed> _executor;
    };
};

TEST_F(ServiceExecutorFixedFixture, ScheduleFailsBeforeStartup) {
    ServiceExecutorHandle executorHandle(ServiceExecutorHandle::kSkipShutdown);
    ASSERT_NOT_OK(executorHandle->scheduleTask([] {}, ServiceExecutor::kEmptyFlags));
}

DEATH_TEST_F(ServiceExecutorFixedFixture, DestructorFailsBeforeShutdown, "invariant") {
    FailPointEnableBlock failpoint("hangAfterServiceExecutorFixedExecutorThreadsStart");
    ServiceExecutorHandle executorHandle(ServiceExecutorHandle::kStartExecutor |
                                         ServiceExecutorHandle::kSkipShutdown);
    // The following ensures `executorHandle` holds the only reference to the service executor, thus
    // returning from this block would trigger destruction of the executor.
    failpoint->waitForTimesEntered(kNumExecutorThreads);
}

TEST_F(ServiceExecutorFixedFixture, BasicTaskRuns) {
    ServiceExecutorHandle executorHandle(ServiceExecutorHandle::kStartExecutor);
    auto barrier = std::make_shared<unittest::Barrier>(2);
    ASSERT_OK(executorHandle->scheduleTask([barrier]() mutable { barrier->countDownAndWait(); },
                                           ServiceExecutor::kEmptyFlags));
    barrier->countDownAndWait();
}

TEST_F(ServiceExecutorFixedFixture, RecursiveTask) {
    ServiceExecutorHandle executorHandle(ServiceExecutorHandle::kStartExecutor);
    auto barrier = std::make_shared<unittest::Barrier>(2);

    std::function<void()> recursiveTask;
    recursiveTask = [&, barrier, executor = *executorHandle] {
        if (executor->getRecursionDepthForExecutorThread() <
            fixedServiceExecutorRecursionLimit.load()) {
            ASSERT_OK(executor->scheduleTask(recursiveTask, ServiceExecutor::kMayRecurse));
        } else {
            // This test never returns unless the service executor can satisfy the recursion depth.
            barrier->countDownAndWait();
        }
    };

    // Schedule recursive task and wait for the recursion to stop
    ASSERT_OK(executorHandle->scheduleTask(recursiveTask, ServiceExecutor::kMayRecurse));
    barrier->countDownAndWait();
}

TEST_F(ServiceExecutorFixedFixture, FlattenRecursiveScheduledTasks) {
    ServiceExecutorHandle executorHandle(ServiceExecutorHandle::kStartExecutor);
    auto barrier = std::make_shared<unittest::Barrier>(2);
    AtomicWord<int> tasksToSchedule{fixedServiceExecutorRecursionLimit.load() * 3};

    // A recursive task that expects to be executed non-recursively. The task recursively schedules
    // "tasksToSchedule" tasks to the service executor, and each scheduled task verifies that the
    // recursion depth remains zero during its execution.
    std::function<void()> recursiveTask;
    recursiveTask = [&, barrier, executor = *executorHandle] {
        ASSERT_EQ(executor->getRecursionDepthForExecutorThread(), 1);
        if (tasksToSchedule.fetchAndSubtract(1) > 0) {
            ASSERT_OK(executor->scheduleTask(recursiveTask, ServiceExecutor::kEmptyFlags));
        } else {
            // Once there are no more tasks to schedule, notify the main thread to proceed.
            barrier->countDownAndWait();
        }
    };

    // Schedule the recursive task and wait for the execution to finish.
    ASSERT_OK(
        executorHandle->scheduleTask(recursiveTask, ServiceExecutor::kMayYieldBeforeSchedule));
    barrier->countDownAndWait();
}

TEST_F(ServiceExecutorFixedFixture, ShutdownTimeLimit) {
    ServiceExecutorHandle executorHandle(ServiceExecutorHandle::kStartExecutor);
    auto invoked = std::make_shared<SharedPromise<void>>();
    auto mayReturn = std::make_shared<SharedPromise<void>>();

    ASSERT_OK(executorHandle->scheduleTask(
        [executor = *executorHandle, invoked, mayReturn]() mutable {
            invoked->emplaceValue();
            mayReturn->getFuture().get();
        },
        ServiceExecutor::kEmptyFlags));

    invoked->getFuture().get();
    ASSERT_NOT_OK(executorHandle->shutdown(kShutdownTime));

    // Ensure the service executor is stopped before leaving the test.
    mayReturn->emplaceValue();
}

TEST_F(ServiceExecutorFixedFixture, Stats) {
    ServiceExecutorHandle executorHandle(ServiceExecutorHandle::kStartExecutor);
    auto rendezvousBarrier = std::make_shared<unittest::Barrier>(kNumExecutorThreads + 1);
    auto returnBarrier = std::make_shared<unittest::Barrier>(kNumExecutorThreads + 1);

    auto task = [rendezvousBarrier, returnBarrier]() mutable {
        rendezvousBarrier->countDownAndWait();
        // Executor threads wait here for the main thread to test "executor->appendStats()".
        returnBarrier->countDownAndWait();
    };

    for (auto i = 0; i < kNumExecutorThreads; i++) {
        ASSERT_OK(executorHandle->scheduleTask(task, ServiceExecutor::kEmptyFlags));
    }

    // The main thread waits for the executor threads to bump up "threadsRunning" while picking up a
    // task to execute. Once all executor threads are running (rendezvous) and the main thread is
    // done testing the stats, the main thread will unblock them through "returnBarrier".
    rendezvousBarrier->countDownAndWait();

    BSONObjBuilder bob;
    executorHandle->appendStats(&bob);
    auto obj = bob.obj();
    ASSERT(obj.hasField("threadsRunning"));
    auto threadsRunning = obj.getIntField("threadsRunning");
    ASSERT_EQ(threadsRunning, static_cast<int>(ServiceExecutorFixedFixture::kNumExecutorThreads));

    returnBarrier->countDownAndWait();
}


TEST_F(ServiceExecutorFixedFixture, ScheduleSucceedsBeforeShutdown) {
    ServiceExecutorHandle executorHandle(ServiceExecutorHandle::kStartExecutor);

    auto thread = stdx::thread();
    auto barrier = std::make_shared<unittest::Barrier>(2);
    {
        FailPointEnableBlock failpoint("hangBeforeSchedulingServiceExecutorFixedTask");


        // The executor accepts the work, but hasn't used the underlying pool yet.
        thread = stdx::thread([&] {
            ASSERT_OK(executorHandle->scheduleTask([&, barrier] { barrier->countDownAndWait(); },
                                                   ServiceExecutor::kEmptyFlags));
        });
        failpoint->waitForTimesEntered(1);

        // Trigger an immediate shutdown which will not affect the task we have accepted.
        ASSERT_NOT_OK(executorHandle->shutdown(Milliseconds{0}));
    }

    // Our failpoint has been disabled, so the task can run to completion.
    barrier->countDownAndWait();

    // Now we can wait for the task to finish and shutdown.
    ASSERT_OK(executorHandle->shutdown(kShutdownTime));

    thread.join();
}

TEST_F(ServiceExecutorFixedFixture, ScheduleFailsAfterShutdown) {
    ServiceExecutorHandle executorHandle(ServiceExecutorHandle::kStartExecutor);

    ASSERT_OK(executorHandle->shutdown(kShutdownTime));
    ASSERT_NOT_OK(
        executorHandle->scheduleTask([] { MONGO_UNREACHABLE; }, ServiceExecutor::kEmptyFlags));
}

TEST_F(ServiceExecutorFixedFixture, RunTaskAfterWaitingForData) {
    auto tl = std::make_unique<TransportLayerMock>();
    auto session = tl->createSession();

    ServiceExecutorHandle executorHandle(ServiceExecutorHandle::kStartExecutor);

    const auto mainThreadId = stdx::this_thread::get_id();
    AtomicWord<bool> ranOnDataAvailable{false};
    auto barrier = std::make_shared<unittest::Barrier>(2);
    executorHandle->runOnDataAvailable(
        session, [&ranOnDataAvailable, mainThreadId, barrier](Status) mutable -> void {
            ranOnDataAvailable.store(true);
            ASSERT(stdx::this_thread::get_id() != mainThreadId);
            barrier->countDownAndWait();
        });

    ASSERT(!ranOnDataAvailable.load());
    reinterpret_cast<MockSession*>(session.get())->signalAvailableData();
    barrier->countDownAndWait();
    ASSERT(ranOnDataAvailable.load());
}

TEST_F(ServiceExecutorFixedFixture, StartAndShutdownAreDeterministic) {

    std::unique_ptr<ServiceExecutorHandle> handle;

    // Ensure starting the executor results in spawning the specified number of executor threads.
    {
        FailPointEnableBlock failpoint("hangAfterServiceExecutorFixedExecutorThreadsStart");
        handle = std::make_unique<ServiceExecutorHandle>(ServiceExecutorHandle::kNone);
        ASSERT_OK((*handle)->start());
        failpoint->waitForTimesEntered(kNumExecutorThreads);
    }

    // Since destroying ServiceExecutorFixed is blocking, spawn a thread to issue the destruction
    // off of the main execution path.
    stdx::thread shutdownThread;

    // Ensure all executor threads return after receiving the shutdown signal.
    {
        FailPointEnableBlock failpoint("hangBeforeServiceExecutorFixedLastExecutorThreadReturns");
        shutdownThread = stdx::thread{[handle = std::move(handle)]() mutable { handle.reset(); }};
        failpoint->waitForTimesEntered(1);
    }
    shutdownThread.join();
}

}  // namespace
}  // namespace mongo
