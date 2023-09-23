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


#include <asio.hpp>  // IWYU pragma: keep
#include <boost/smart_ptr.hpp>
#include <chrono>
#include <compare>
#include <cstddef>
#include <memory>
#include <new>
#include <thread>
#include <utility>

// IWYU pragma: no_include "asio/impl/dispatch.hpp"
// IWYU pragma: no_include "asio/impl/io_context.hpp"
// IWYU pragma: no_include "asio/impl/post.hpp"
// IWYU pragma: no_include "asio/impl/system_executor.hpp"
#include <asio/io_context.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_fixed.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/matcher_core.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::transport {
namespace {

namespace m = unittest::match;

constexpr auto kWorkerThreadRunTime = Milliseconds{1000};
// Run time + generous scheduling time slice
constexpr auto kShutdownTime = Milliseconds{kWorkerThreadRunTime.count() + 50};

class JoinThread : public stdx::thread {
public:
    using stdx::thread::thread;
    ~JoinThread() {
        if (joinable())
            join();
    }
};

/* This implements the portions of the transport::Reactor based on ASIO, but leaves out
 * the methods not needed by ServiceExecutors.
 *
 * TODO Maybe use AsioTransportLayer's Reactor?
 */
class AsioReactor : public transport::Reactor {
public:
    AsioReactor() : _ioContext() {}

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
    ServiceExecutorSynchronous executor(getGlobalServiceContext());
    ASSERT_OK(executor.start());
    PromiseAndFuture<void> pf;
    auto runner = executor.makeTaskRunner();
    runner->schedule([&](Status st) { pf.promise.setFrom(st); });
    ASSERT_DOES_NOT_THROW(pf.future.get());
    ASSERT_OK(executor.shutdown(kShutdownTime));
}

TEST_F(ServiceExecutorSynchronousTest, MakeTaskRunnerFailsBeforeStartup) {
    ServiceExecutorSynchronous executor{getGlobalServiceContext()};
    ASSERT_THROWS(executor.makeTaskRunner(), DBException);
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

TEST_F(ServiceExecutorFixedTest, MakeTaskRunnerFailsBeforeStartup) {
    Handle handle;
    ASSERT_THROWS(handle->makeTaskRunner(), DBException);
}

TEST_F(ServiceExecutorFixedTest, BasicTaskRuns) {
    Handle handle;
    handle.start();
    auto runner = handle->makeTaskRunner();
    PromiseAndFuture<void> pf;
    runner->schedule([&](Status s) { pf.promise.setFrom(s); });
    ASSERT_DOES_NOT_THROW(pf.future.get());
}

TEST_F(ServiceExecutorFixedTest, ShutdownTimeLimit) {
    unittest::Barrier mayReturn(2);
    Handle handle;
    handle.start();
    auto runner = handle->makeTaskRunner();
    PromiseAndFuture<void> pf;
    runner->schedule([&](Status st) {
        pf.promise.setFrom(st);
        mayReturn.countDownAndWait();
    });
    ASSERT_DOES_NOT_THROW(pf.future.get());
    ASSERT_NOT_OK(handle->shutdown(kShutdownTime));

    // Ensure the service executor is stopped before leaving the test.
    mayReturn.countDownAndWait();
}

TEST_F(ServiceExecutorFixedTest, ScheduleSucceedsBeforeShutdown) {
    boost::optional<FailPointEnableBlock> failpoint("hangBeforeSchedulingServiceExecutorFixedTask");
    PromiseAndFuture<void> pf;
    Handle handle;
    handle.start();
    auto runner = handle->makeTaskRunner();

    // The executor accepts the work, but hasn't used the underlying pool yet.
    JoinThread scheduleClient{[&] {
        runner->schedule([&](Status s) { pf.promise.setFrom(s); });
    }};
    (*failpoint)->waitForTimesEntered(failpoint->initialTimesEntered() + 1);

    // Trigger an immediate shutdown which will not affect the task we have accepted.
    ASSERT_NOT_OK(handle->shutdown(Milliseconds{0}));
    failpoint.reset();

    // Our failpoint has been disabled, so the task can run to completion.
    ASSERT_DOES_NOT_THROW(pf.future.get());

    // Now we can wait for the task to finish and shutdown.
    ASSERT_OK(handle->shutdown(kShutdownTime));
}

TEST_F(ServiceExecutorFixedTest, ScheduleFailsAfterShutdown) {
    Handle handle;
    handle.start();
    auto runner = handle->makeTaskRunner();
    ASSERT_OK(handle->shutdown(kShutdownTime));
    PromiseAndFuture<void> pf;
    runner->schedule([&](Status s) { pf.promise.setFrom(s); });
    ASSERT_THROWS(pf.future.get(), ExceptionFor<ErrorCodes::ServiceExecutorInShutdown>);
}

TEST_F(ServiceExecutorFixedTest, RunTaskAfterWaitingForData) {
    unittest::threadAssertionMonitoredTest([&](auto&& monitor) {
        unittest::Barrier barrier(2);
        auto tl = std::make_unique<TransportLayerMock>();
        auto session = std::dynamic_pointer_cast<MockSession>(tl->createSession());
        invariant(session);

        Handle handle;
        handle.start();
        auto runner = handle->makeTaskRunner();

        const auto signallingThreadId = stdx::this_thread::get_id();

        AtomicWord<bool> ranOnDataAvailable{false};

        runner->runOnDataAvailable(session, [&](Status) {
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
            failpoint->waitForTimesEntered(failpoint.initialTimesEntered() + kExecutorThreads);
        }

        // Since destroying ServiceExecutorFixed is blocking, spawn a thread to issue the
        // destruction off of the main execution path.
        stdx::thread shutdownThread;

        // Ensure all executor threads return after receiving the shutdown signal.
        {
            FailPointEnableBlock failpoint(
                "hangBeforeServiceExecutorFixedLastExecutorThreadReturns");
            shutdownThread = monitor.spawn([&] { handle.join(); });
            failpoint->waitForTimesEntered(failpoint.initialTimesEntered() + 1);
        }
        shutdownThread.join();
    });
}

}  // namespace
}  // namespace mongo::transport
