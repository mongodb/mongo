// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include <chrono>
#include <compare>
#include <cstddef>
#include <memory>
#include <new>
#include <thread>
#include <utility>

#include <asio.hpp>  // IWYU pragma: keep

#include <boost/smart_ptr.hpp>

// IWYU pragma: no_include "asio/impl/dispatch.hpp"
// IWYU pragma: no_include "asio/impl/io_context.hpp"
// IWYU pragma: no_include "asio/impl/post.hpp"
// IWYU pragma: no_include "asio/impl/system_executor.hpp"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"

#include <asio/io_context.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::transport {
namespace {

namespace m = unittest::match;

constexpr auto kWorkerThreadRunTime = Milliseconds{1000};
// Run time + generous scheduling time slice
constexpr auto kShutdownTime = Milliseconds{kWorkerThreadRunTime.count() + 50};


/* This implements the portions of the transport::Reactor based on ASIO, but leaves out
 * the methods not needed by ServiceExecutors.
 *
 * TODO Maybe use AsioTransportLayer's Reactor?
 */
class AsioReactor : public transport::Reactor {
public:
    AsioReactor() : _ioContext() {}

    void run() final {
        MONGO_UNREACHABLE;
    }

    void stop() final {
        _ioContext.stop();
    }

    void drain() final {
        _ioContext.restart();
        while (_ioContext.poll()) {
            LOGV2_DEBUG(22984, 1, "Draining remaining work in reactor.");
        }
        _ioContext.stop();
    }

    std::unique_ptr<ReactorTimer> makeTimer() final {
        MONGO_UNREACHABLE;
    }

    std::chrono::system_clock::time_point systemTime() final {
        MONGO_UNREACHABLE;
    }

    void schedule(Task task) final {
        asio::post(_ioContext, [task = std::move(task)] { task(Status::OK()); });
    }

    operator asio::io_context&() {
        return _ioContext;
    }

private:
    asio::io_context _ioContext;
};

class ServiceExecutorInlineTest : public unittest::Test {
public:
    ServiceExecutorInline executor;
};

class ServiceExecutorSynchronousTest : public unittest::Test {
public:
    ServiceExecutorSynchronous executor;
};

TEST_F(ServiceExecutorInlineTest, MakeTaskRunnerFailsBeforeStartup) {
    ASSERT_THROWS(executor.makeTaskRunner(), DBException);
}

TEST_F(ServiceExecutorSynchronousTest, MakeTaskRunnerFailsBeforeStartup) {
    ASSERT_THROWS(executor.makeTaskRunner(), DBException);
}

// Schedule a task and ensure it has been executed.
stdx::thread::id doBasicTaskRunTest(ServiceExecutor* executor) {
    boost::optional<stdx::thread::id> taskid;
    executor->start();
    auto runner = executor->makeTaskRunner();
    PromiseAndFuture<void> pf;
    runner->schedule([&](Status st) {
        taskid = std::this_thread::get_id();
        pf.promise.setFrom(st);
    });
    ASSERT_DOES_NOT_THROW(pf.future.get());
    ASSERT_OK(executor->shutdown(kShutdownTime));
    ASSERT(!!taskid);
    return *taskid;
}

TEST_F(ServiceExecutorSynchronousTest, BasicTaskRuns) {
    auto callerid = std::this_thread::get_id();
    auto taskid = doBasicTaskRunTest(&executor);
    // Task runs on different thread than caller.
    ASSERT(callerid != taskid);
}

TEST_F(ServiceExecutorInlineTest, BasicTaskRuns) {
    auto callerid = std::this_thread::get_id();
    auto taskid = doBasicTaskRunTest(&executor);
    // Task runs on same thread as caller.
    ASSERT(callerid == taskid);
}

/** Implements a threadsafe 1-shot pause and resume. */
class Breakpoint {
public:
    void pause() {
        _paused.set();
        _resumed.get();
    }

    void await() {
        _paused.get();
    }

    void resume() {
        _resumed.set();
    }

private:
    Notification<void> _paused;
    Notification<void> _resumed;
};

TEST_F(ServiceExecutorSynchronousTest, SpawnsWorkerThread) {
    synchronized_value<std::vector<std::string>> events;

    executor.start();
    PromiseAndFuture<void> pf;
    auto runner = executor.makeTaskRunner();

    // Expect ServiceExecutorSynchronous to schedule on a worker thread allowing
    // "caller" to be pushed onto the events vector once the task blocks on its breakpoint.
    // If the task executes in the caller's thread, then bp.pause() blocks indefinitely.
    // If the task thread never executes, then bp.await() blocks indefinitely.
    {
        Breakpoint bp;
        runner->schedule([&](Status st) {
            bp.pause();
            events->push_back("task");
            pf.promise.setFrom(st);
        });
        bp.await();
        events->push_back("caller");
        bp.resume();
        ASSERT_DOES_NOT_THROW(pf.future.get());
    }
    ASSERT_OK(executor.shutdown(kShutdownTime));

    ASSERT_THAT(**events, m::ElementsAre(m::Eq("caller"), m::Eq("task")));
}

// Ensure that tasks queued during the running of a task are executed
// in the order they are enqueued.
void doTestTaskQueueing(ServiceExecutor* executor) {
    synchronized_value<std::vector<int>> events;

    executor->start();
    PromiseAndFuture<void> pf;
    auto runner = executor->makeTaskRunner();

    runner->schedule([&](Status st) {
        for (int i = 2; i < 5; ++i) {
            runner->schedule([&, i](Status st) { events->push_back(i); });
        }
        runner->schedule([&](Status st) { pf.promise.setFrom(st); });
        events->push_back(1);
    });
    ASSERT_DOES_NOT_THROW(pf.future.get());
    ASSERT_OK(executor->shutdown(kShutdownTime));

    ASSERT_THAT(**events, m::ElementsAre(m::Eq(1), m::Eq(2), m::Eq(3), m::Eq(4)));
}

TEST_F(ServiceExecutorSynchronousTest, TaskQueueing) {
    doTestTaskQueueing(&executor);
}

TEST_F(ServiceExecutorInlineTest, TaskQueueing) {
    doTestTaskQueueing(&executor);
}

/** Ensure that tasks queued after a task queue has emptied will still run. */
void doTestTaskPostQueueing(ServiceExecutor* executor) {
    executor->start();
    auto runner = executor->makeTaskRunner();

    PromiseAndFuture<void> first;
    runner->schedule([&](Status st) { first.promise.setFrom(st); });
    ASSERT_DOES_NOT_THROW(first.future.get());

    if (dynamic_cast<ServiceExecutorInline*>(executor)) {
        ASSERT_EQ(executor->getRunningThreads(), 0);
    } else {
        // In the case of ServiceExecutorInline we know the queue is empty after
        // the first schedule call since it blocks, but we don't know this is
        // true for ServiceExecutorSynchronous, so we potentially need to wait.
        // Don't wait longer than 10 seconds though.
        auto endWait = Date_t::now() + Seconds{10};
        while (executor->getRunningThreads() > 0) {
            sleepFor(Milliseconds{10});
            ASSERT_LT(Date_t::now(), endWait);
        }
    }

    PromiseAndFuture<void> second;
    runner->schedule([&](Status st) { second.promise.setFrom(st); });
    ASSERT_DOES_NOT_THROW(second.future.get());

    ASSERT_OK(executor->shutdown(kShutdownTime));
}

TEST_F(ServiceExecutorSynchronousTest, TaskPostQueueing) {
    doTestTaskPostQueueing(&executor);
}

TEST_F(ServiceExecutorInlineTest, TaskPostQueueing) {
    doTestTaskPostQueueing(&executor);
}

}  // namespace
}  // namespace mongo::transport
