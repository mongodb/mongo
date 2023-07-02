/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/waitable.h"

namespace mongo::executor {
namespace {

class InlineExecutorTest : public ServiceContextTest {
public:
    static constexpr size_t kNumTasks = 10;

    void stop() {
        _ie.reset();
    }

    void setUp() override {
        _ie = std::make_unique<InlineExecutor>();
    }

    void tearDown() override {
        stop();
    }

    InlineExecutor& getInlineExecutor() {
        return *_ie;
    }

    auto getExecutor() {
        return getInlineExecutor().getExecutor();
    }

    void runScheduledTasks() {
        Notification<void> mayReturn;
        // The following is marker to ensure `_ie` executes all tasks scheduled before this point.
        getExecutor()->schedule([&](Status) { mayReturn.set(); });
        getInlineExecutor().run([&] { return !!mayReturn; });
    }

private:
    std::unique_ptr<InlineExecutor> _ie;
};

TEST_F(InlineExecutorTest, SimpleTask) {
    auto pf = makePromiseFuture<void>();
    getExecutor()->schedule([promise = std::move(pf.promise)](Status status) mutable {
        promise.setWith([&] { return status; });
    });

    ASSERT_FALSE(pf.future.isReady());
    runScheduledTasks();
    ASSERT_TRUE(pf.future.isReady());
    ASSERT_OK(pf.future.getNoThrow());
}

TEST_F(InlineExecutorTest, TasksRunInOrder) {
    size_t executed = 0;
    for (size_t i = 0; i < kNumTasks; i++) {
        getExecutor()->schedule([&, order = i](Status) { ASSERT_EQ(executed++, order); });
    }
    runScheduledTasks();
    ASSERT_EQ(executed, kNumTasks);
}

TEST_F(InlineExecutorTest, Predicate) {
    size_t executed = 0;
    Notification<void> mayReturn;

    for (size_t i = 0; i < kNumTasks; i++) {
        getExecutor()->schedule([&](Status) {
            executed++;
            if (executed == kNumTasks)
                mayReturn.set();
        });
    }

    getInlineExecutor().run([&] { return !!mayReturn; });
    ASSERT_EQ(executed, kNumTasks);
}

TEST_F(InlineExecutorTest, ShutdownDrainsScheduledTasks) {
    size_t scheduled = 0, executed = 0;
    for (size_t i = 0; i < kNumTasks; i++, scheduled++) {
        getExecutor()->schedule([&](Status status) {
            ASSERT_EQ(status.code(), ErrorCodes::ShutdownInProgress);
            executed++;
        });
    }

    ASSERT_EQ(scheduled, kNumTasks);
    ASSERT_EQ(executed, 0);

    stop();
    ASSERT_EQ(executed, scheduled);
}

TEST_F(InlineExecutorTest, ScheduleDuringShutdown) {
    // The barriers ensure the worker always tries to schedule its task while shutdown is in
    // progress (i.e., the destructor for `InlineExecutor` is draining scheduled tasks).
    unittest::Barrier b1{2}, b2{2};
    auto executor = getExecutor();
    executor->schedule([&](Status) {
        b1.countDownAndWait();
        b2.countDownAndWait();
    });

    unittest::ThreadAssertionMonitor monitor;
    auto worker = monitor.spawn([&] {
        b1.countDownAndWait();
        auto pf = makePromiseFuture<void>();
        executor->schedule([&](Status status) { pf.promise.setWith([&] { return status; }); });
        ASSERT_THROWS_CODE(pf.future.get(), DBException, ErrorCodes::ShutdownInProgress);
        b2.countDownAndWait();
        monitor.notifyDone();
    });

    stop();
    worker.join();
}

TEST_F(InlineExecutorTest, ScheduleAfterShutdown) {
    auto executor = getExecutor();
    stop();
    executor->schedule([](Status status) { ASSERT_EQ(status, ErrorCodes::ShutdownInProgress); });
}

TEST_F(InlineExecutorTest, TasksRunInline) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        auto id = stdx::thread::id();
        monitor
            .spawn([&] {
                getExecutor()->schedule([&](Status) { ASSERT_EQ(stdx::thread::id(), id); });
            })
            .join();
        runScheduledTasks();
    });
}

TEST_F(InlineExecutorTest, MultipleSchedulers) {
    const size_t kThreads = 10;
    size_t executed = 0;  // No need to synchronize as this is only accessed by the main thread.

    std::vector<stdx::thread> threads;
    for (size_t i = 0; i < kThreads; i++) {
        threads.emplace_back([&] {
            for (size_t i = 0; i < kNumTasks; i++)
                getExecutor()->schedule([&](Status) { executed++; });
        });
    }

    getInlineExecutor().run([&] { return executed == kThreads * kNumTasks; });

    for (size_t i = 0; i < kThreads; i++)
        threads[i].join();
}

TEST_F(InlineExecutorTest, Interruptible) {
    auto client = getServiceContext()->makeClient("InlineExecutorTest");
    auto opCtx = client->makeOperationContext();
    opCtx->markKilled();
    ASSERT_THROWS_CODE(getInlineExecutor().run([] { return false; }, opCtx.get()),
                       DBException,
                       ErrorCodes::Interrupted);
}

class SleepableExecutorTest : public ThreadPoolExecutorTest {
public:
    void setUp() override {
        ThreadPoolExecutorTest::setUp();
        launchExecutorThread();
    }

    /*
     * Helper to run most of the tests for this fixture, which blocks until the semi-future produced
     * by the provided `FutureFactory` becomes ready. This helper expects the semi-future to be
     * initially non-ready, and become ready after a task is successfully executed by the instance
     * of `InlineExecutor`.
     */
    using FutureFactory = std::function<SemiFuture<void>(InlineExecutor&)>;
    void runTest(FutureFactory factory) {
        InlineExecutor ie;
        auto future = factory(ie);
        ASSERT_FALSE(future.isReady());
        ie.run([&] { return future.isReady(); });
        ASSERT_DOES_NOT_THROW(future.get());
    }
};

DEATH_TEST_F(SleepableExecutorTest, NoExecutor, "invariant") {
    InlineExecutor ie;
    auto se = ie.getSleepableExecutor(nullptr);
}

TEST_F(SleepableExecutorTest, SimpleTask) {
    runTest([executor = getExecutorPtr()](InlineExecutor& ie) {
        auto pf = makePromiseFuture<void>();
        ie.getSleepableExecutor(executor)->schedule(
            [promise = std::move(pf.promise)](Status status) mutable {
                promise.setWith([&] { return status; });
            });
        return std::move(pf.future).semi();
    });
}

TEST_F(SleepableExecutorTest, WithExecutor) {
    runTest([executor = getExecutorPtr(), net = getNet()](InlineExecutor& ie) {
        auto se = ie.getSleepableExecutor(executor);
        se->schedule([net](Status) {
            NetworkInterfaceMock::InNetworkGuard guard(net);
            net->runUntil(net->now() + Seconds(1));
        });
        return se->sleepFor(Seconds(1), CancellationToken::uncancelable()).semi();
    });
}

TEST_F(SleepableExecutorTest, WithNetworkingBaton) {
    /*
     * The following wraps a `TaskExecutor` to emulate a networking baton, in particular its
     * `waitUntil` functionality that is internally used by `SleepableExecutor`.
     */
    class DummyBaton : public transport::NetworkingBaton {
    public:
        explicit DummyBaton(std::shared_ptr<TaskExecutor> executor)
            : _executor(std::move(executor)) {}

        Future<void> waitUntil(Date_t when, const CancellationToken& token) noexcept override {
            auto pf = makePromiseFuture<void>();
            _executor->sleepUntil(when, token)
                .getAsync([promise = std::move(pf.promise)](Status status) mutable {
                    promise.setWith([&] { return status; });
                });
            return std::move(pf.future);
        }

        // The rest of the methods are not implemented.
        void notify() noexcept override {
            MONGO_UNIMPLEMENTED;
        }
        void run(ClockSource*) noexcept override {
            MONGO_UNIMPLEMENTED;
        }
        TimeoutState run_until(ClockSource*, Date_t) noexcept override {
            MONGO_UNIMPLEMENTED;
        }
        void schedule(Task) noexcept override {
            MONGO_UNIMPLEMENTED;
        }
        void markKillOnClientDisconnect() noexcept override {
            MONGO_UNIMPLEMENTED;
        }
        Future<void> addSession(transport::Session&, Type) noexcept override {
            MONGO_UNIMPLEMENTED;
        }
        Future<void> waitUntil(const transport::ReactorTimer&, Date_t) noexcept override {
            MONGO_UNIMPLEMENTED;
        }
        bool cancelSession(transport::Session&) noexcept override {
            MONGO_UNIMPLEMENTED;
        }
        bool cancelTimer(const transport::ReactorTimer&) noexcept override {
            MONGO_UNIMPLEMENTED;
        }
        bool canWait() noexcept override {
            MONGO_UNIMPLEMENTED;
        }
        void detachImpl() noexcept override {
            MONGO_UNIMPLEMENTED;
        }

    private:
        std::shared_ptr<TaskExecutor> _executor;
    };

    auto baton = std::make_shared<DummyBaton>(getExecutorPtr());
    runTest([baton, net = getNet()](InlineExecutor& ie) {
        auto se = ie.getSleepableExecutor(nullptr, baton);
        se->schedule([net](Status) {
            NetworkInterfaceMock::InNetworkGuard guard(net);
            net->runUntil(Date_t::now() + Seconds(5));
        });
        return se->sleepFor(Seconds(5), CancellationToken::uncancelable()).semi();
    });
}

/**
 * Example for using `AsyncTry` with `InlineExecutor`. Note that the instance of `InlineExecutor`
 * must remain alive so long as `AsyncTry` is scheduling new iterations (i.e., the returned future
 * is not ready).
 */
TEST_F(SleepableExecutorTest, AsyncTry) {
    int i = 0;
    const int kIterations = 100;
    runTest([&](InlineExecutor& ie) {
        return AsyncTry([&] { i++; })
            .until([&](Status) { return i == kIterations; })
            .on(ie.getSleepableExecutor(getExecutorPtr()), CancellationToken::uncancelable())
            .semi();
    });
    ASSERT_EQ(i, kIterations);
}

}  // namespace
}  // namespace mongo::executor
