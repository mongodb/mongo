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
#include <asio.hpp>

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
#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/matcher.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/synchronized_value.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::transport {
namespace {

namespace m = unittest::match;

using unittest::stringify::stringifyForAssert;

constexpr auto kWorkerThreadRunTime = Milliseconds{1000};
// Run time + generous scheduling time slice
constexpr auto kShutdownTime = Milliseconds{kWorkerThreadRunTime.count() + 50};

// Fibonacci generator for slow integer-valued exponential backoff.
template <typename T>
auto fibGenerator() {
    return [seq = std::array{T{0}, T{1}}]() mutable {
        auto r = seq[0];
        seq[0] = seq[1];
        seq[1] = r + seq[0];
        return r;
    };
}

template <typename Pred>
auto pollUntil(const Pred& pred, Milliseconds timeout) {
    auto fib = fibGenerator<Milliseconds>();
    while (true) {
        if (auto r = pred(); r || timeout == Milliseconds{0})
            return r;
        auto zzz = std::min(fib(), timeout);
        timeout -= zzz;
        sleepFor(zzz);
    }
}

template <typename M>
class AtomicWordLoadIs : public m::Matcher {
public:
    explicit AtomicWordLoadIs(M m) : _m{std::move(m)} {}

    std::string describe() const {
        return "AtomicWordLoadIs({})"_format(_m.describe());
    }

    template <typename T>
    m::MatchResult match(const T& x) const {
        auto r = x.load();
        if (auto mr = _m.match(r); !mr)
            return {false,
                    "{} failed {}{}"_format(stringifyForAssert(r), _m.describe(), mr.message())};
        return {};
    }

private:
    M _m;
};

/** Matches a ServiceExecutor or the BSONObj it produces with its `appendStats`. */
template <typename ThreadMatch, typename ClientMatch>
class ExecStatsIs : public m::Matcher {
public:
    ExecStatsIs(std::string execStatsLabel, ThreadMatch tm, ClientMatch cm)
        : _execStatsLabel{std::move(execStatsLabel)}, _tm{std::move(tm)}, _cm{std::move(cm)} {}

    std::string describe() const {
        return "ExecStatsIs({},{})"_format(_tm.describe(), _cm.describe());
    }

    m::MatchResult match(const BSONObj& x) const {
        unittest::stringify::Joiner joiner;
        bool ok = true;
        auto obj = x[_execStatsLabel].Obj();

        auto tIn = obj["threadsRunning"].Int();
        if (auto tmr = _tm.match(tIn); !tmr) {
            joiner("threadsRunning={} failed {}{}"_format(
                stringifyForAssert(tIn), _tm.describe(), tmr.message()));
            ok = false;
        }

        auto cIn = obj["clientsInTotal"].Int();
        if (auto cmr = _cm.match(cIn); !cmr) {
            joiner("clientsInTotal={} failed {}{}"_format(
                stringifyForAssert(cIn), _cm.describe(), cmr.message()));
            ok = false;
        }
        return {ok, std::string{joiner}};
    }

    m::MatchResult match(const ServiceExecutor& exec) const {
        BSONObjBuilder bob;
        exec.appendStats(&bob);
        BSONObj obj = bob.done();
        if (auto mr = match(obj); !mr)
            return {false, "obj={}, message={}"_format(obj.toString(), mr.message())};
        return {};
    }

private:
    std::string _execStatsLabel;
    ThreadMatch _tm;
    ClientMatch _cm;
};

/**
 * Match is re-evaluated repeatedly with an exponential backoff, up to some
 * limit, at which time this enclosing matcher fails.
 */
template <typename M>
class SoonMatches : public m::Matcher {
public:
    explicit SoonMatches(M&& m, Milliseconds timeout = Seconds{5})
        : _m{std::forward<M>(m)}, _timeout{timeout} {}

    std::string describe() const {
        return "SoonMatches({},{})"_format(_m.describe(), _timeout.toString());
    }

    template <typename X>
    m::MatchResult match(const X& x) const {
        auto mr = pollUntil([&] { return _m.match(x); }, _timeout);
        if (mr)
            return mr;
        return {false, "No result matched after {}: {}"_format(_timeout.toString(), mr.message())};
    }

private:
    M _m;
    Milliseconds _timeout;
};

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

/**
 * ServiceExecutorSynchronous and ServiceExecutorReserved are closely related.
 * This is a common basis for the fixtures that test them.
 */
template <typename Derived>
class ServiceExecutorSynchronousTestBase : public unittest::Test {
public:
    auto execStatsElementMatcher(int threads, int clients) {
        return ExecStatsIs(getStatsLabel(), m::Eq(threads), m::Eq(clients));
    }

    void testCreateDestroy() {
        makeExecutor();
    }

    void testStartStop() {
        auto executor = makeExecutor();
        ASSERT_OK(executor.start());
        ASSERT_OK(executor.shutdown(kShutdownTime));
    }

    void testMakeTaskRunnerFailsBeforeStartup() {
        auto executor = makeExecutor();
        ASSERT_THROWS(executor.makeTaskRunner(), DBException);
    }

    void testMakeTaskRunner() {
        auto executor = makeExecutor();
        ASSERT_OK(executor.start());
        executor.makeTaskRunner();
        ASSERT_OK(executor.shutdown(kShutdownTime));
    }

    void testMakeTaskRunnerMultiple() {
        auto reserved = getReserved();
        auto executor = makeExecutor();
#define LOCAL_CHECK_STATS(threads, clients) \
    ASSERT_THAT(executor, SoonMatches(execStatsElementMatcher(threads, clients)))
        ASSERT_OK(executor.start());
        LOCAL_CHECK_STATS(reserved, 0);
        std::vector<std::unique_ptr<ServiceExecutor::Executor>> runners;
        // Add a few more beyond the reserve.
        for (size_t i = 0; i < reserved + 3; ++i) {
            runners.push_back(executor.makeTaskRunner());
            LOCAL_CHECK_STATS(runners.size() + reserved, runners.size()) << ", i:" << i;
        }
        ASSERT_OK(executor.shutdown(kShutdownTime));
        LOCAL_CHECK_STATS(0, 0);
#undef LOCAL_CHECK_STATS
    }

    void testBasicTaskRuns() {
        auto executor = makeExecutor();
        ASSERT_OK(executor.start());
        PromiseAndFuture<void> pf;
        auto runner = executor.makeTaskRunner();
        runner->schedule([&](Status st) { pf.promise.setFrom(st); });
        ASSERT_DOES_NOT_THROW(pf.future.get());
        ASSERT_OK(executor.shutdown(kShutdownTime));
    }

    void testShutdownTimeout() {
        auto executor = makeExecutor();
        ASSERT_OK(executor.start());
        auto runner = executor.makeTaskRunner();
        PromiseAndFuture<void> taskStarted;
        runner->schedule([&](Status st) {
            taskStarted.promise.setFrom(st);
            sleepFor(Milliseconds{2000});
        });
        taskStarted.future.get();
        ASSERT_THAT(executor.shutdown(Milliseconds{1000}),
                    m::StatusIs(m::Eq(ErrorCodes::ExceededTimeLimit), m::Any()));
    }

    // Should tolerate the failure to spawn all these reserved threads.
    void testManyLeases() {
        auto executor = makeExecutor();
        ASSERT_OK(executor.start());
        for (size_t i = 0; i < 10; ++i) {
            std::vector<std::unique_ptr<ServiceExecutor::Executor>> leases;
            for (size_t j = 0; j < 20; ++j)
                leases.push_back(executor.makeTaskRunner());
        }
    }

    /**
     * Verify that a new connection cannot get a lease on a worker while it
     * is still busy with a task, even if its lease has been destroyed.
     * Destroys lease from within task, as SessionWorkflow does.
     *
     * Exploits the implementation detail that this kind of ServiceExecutor will
     * reuse the most recently released worker first, and that workers are
     * lazily created. So the worker held by `lease` is only worker when
     * `nextLease` is created.
     */
    void testDelayedEnd() {
        auto svcExec = makeExecutor();
        ASSERT_OK(svcExec.start());

        Notification<bool> pass;
        Notification<void> destroyedLease;
        Notification<void> nextLeaseStarted;

        auto lease = svcExec.makeTaskRunner();
        lease->schedule([&](Status) {
            lease = {};
            destroyedLease.set();
            pass.set(pollUntil([&] { return !!nextLeaseStarted; }, Milliseconds{2000}));
        });

        destroyedLease.get();
        auto nextLease = svcExec.makeTaskRunner();
        nextLease->schedule([&](Status) { nextLeaseStarted.set(); });

        ASSERT(pass.get()) << "worker was reused while running a task";
        ASSERT_OK(svcExec.shutdown(Seconds{10}));
    }

    decltype(auto) makeExecutor() {
        return _d().makeExecutor();
    }

    virtual std::string getStatsLabel() const = 0;

    virtual size_t getReserved() const = 0;

    std::unique_ptr<FailPointEnableBlock> makeFailSpawnBlock() {
        return std::make_unique<FailPointEnableBlock>(
            "serviceExecutorSynchronousThreadFailToSpawn");
    }

private:
    decltype(auto) _d() const {
        return static_cast<const Derived&>(*this);
    }
    decltype(auto) _d() {
        return static_cast<Derived&>(*this);
    }
};

class ServiceExecutorSynchronousTest
    : public ServiceExecutorSynchronousTestBase<ServiceExecutorSynchronousTest> {
public:
    ServiceExecutorSynchronous makeExecutor() const {
        return {};
    }
    std::string getStatsLabel() const override {
        return "passthrough";
    }
    size_t getReserved() const override {
        return 0;
    }
};

class ServiceExecutorReservedTest
    : public ServiceExecutorSynchronousTestBase<ServiceExecutorReservedTest> {
public:
    ServiceExecutorReserved makeExecutor() const {
        return {"testReserved", reserved, maxIdleThreads};
    }
    std::string getStatsLabel() const override {
        return "reserved";
    }
    size_t getReserved() const override {
        return reserved;
    }

protected:
    size_t reserved = 5;
    size_t maxIdleThreads = 0;
};

#define SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASE(fixture, case) \
    TEST_F(fixture, case) {                                          \
        test##case ();                                               \
    }

/**
 * Expand this macro to instantiate the test cases for each of the corresponding
 * member functions of the fixture base class. These are tests that
 * ServiceExecutorSynchronous and ServiceExecutorReserved should pass.
 */
#define SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASES(fixture)                              \
    SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASE(fixture, CreateDestroy)                    \
    SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASE(fixture, StartStop)                        \
    SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASE(fixture, BasicTaskRuns)                    \
    SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASE(fixture, MakeTaskRunnerFailsBeforeStartup) \
    SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASE(fixture, MakeTaskRunner)                   \
    SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASE(fixture, MakeTaskRunnerMultiple)           \
    SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASE(fixture, ShutdownTimeout)                  \
    SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASE(fixture, ManyLeases)                       \
    SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASE(fixture, DelayedEnd)                       \
    /**/

SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASES(ServiceExecutorSynchronousTest)
SERVICE_EXECUTOR_SYNCHRONOUS_COMMON_TEST_CASES(ServiceExecutorReservedTest)

#define SERVICE_EXECUTOR_RESERVED_TEST_CHECK_EXEC_STATS(exec, threads, clients) \
    ASSERT_THAT(exec, SoonMatches(execStatsElementMatcher(threads, clients)))

// Create leases until leases exceed the reserve count of threads, and check
// that the executor keeps its thread count above the number of leases by a
// margin of `reserved` as it goes.
TEST_F(ServiceExecutorReservedTest, CreateLeaseBeyondReserve) {
#define LOCAL_CHECK_STATS(threads, clients) \
    SERVICE_EXECUTOR_RESERVED_TEST_CHECK_EXEC_STATS(executor, threads, clients)
    reserved = 5;
    auto executor = makeExecutor();
    ASSERT_OK(executor.start());
    std::vector<std::unique_ptr<ServiceExecutor::Executor>> leases;
    while (leases.size() < reserved + 1) {
        leases.push_back(executor.makeTaskRunner());
        LOCAL_CHECK_STATS(leases.size() + reserved, leases.size());
    }
    while (!leases.empty()) {
        leases.pop_back();
        LOCAL_CHECK_STATS(leases.size() + reserved, leases.size());
    }
    ASSERT_OK(executor.shutdown(kShutdownTime));
    LOCAL_CHECK_STATS(0, 0);
#undef LOCAL_CHECK_STATS
}

TEST_F(ServiceExecutorReservedTest, ImmediateThrowFromNoReserveSpawnFailure) {
    reserved = 0;
    auto executor = makeExecutor();
    ASSERT_OK(executor.start());
    auto failSpawns = makeFailSpawnBlock();
    ASSERT_THAT(executor, SoonMatches(execStatsElementMatcher(reserved, 0)));
    ASSERT_THROWS(executor.makeTaskRunner(), ExceptionFor<ErrorCodes::InternalError>);
    failSpawns = {};
    ASSERT_DOES_NOT_THROW(executor.makeTaskRunner());
}

// The basic point of the "reserved" ServiceExecutor is to allow new connections
// during time periods in which spawns are failing. Verify this fundamental
// requirement of the reserved ServiceExecutor.
TEST_F(ServiceExecutorReservedTest, ReserveMitigatesSpawnFailures) {
    reserved = 5;
    auto executor = makeExecutor();
    ASSERT_OK(executor.start());
    ASSERT_THAT(executor, execStatsElementMatcher(reserved, 0));
    auto failSpawns = makeFailSpawnBlock();
    std::vector<std::unique_ptr<ServiceExecutor::Executor>> leases;
    while (leases.size() < reserved)
        leases.push_back(executor.makeTaskRunner());
    // One worker is in the starting state while it unsuccesfully attempts to spawn.
    // After the failure, we expect it to be removed from the starting bucket.
    ASSERT_THAT(executor, SoonMatches(execStatsElementMatcher(reserved, leases.size())))
        << "Should be sufficient reserve threads for demand during setup";
    ASSERT_THROWS(executor.makeTaskRunner(), ExceptionFor<ErrorCodes::InternalError>)
        << "Should throw when out of reserve threads";
    failSpawns = {};
    ASSERT_DOES_NOT_THROW(executor.makeTaskRunner());
}

// Check that workers are kept alive after their leases expire according to maxIdleThreads.
TEST_F(ServiceExecutorReservedTest, MaxIdleThreads) {
    for (reserved = 0; reserved != 5; ++reserved) {
        for (maxIdleThreads = 0; maxIdleThreads != 5; ++maxIdleThreads) {
            for (size_t leaseCount = 0; leaseCount != reserved + maxIdleThreads; ++leaseCount) {
                auto executor = makeExecutor();
                ASSERT_OK(executor.start());
                ASSERT_THAT(executor, execStatsElementMatcher(reserved, 0));

                std::vector<std::unique_ptr<ServiceExecutor::Executor>> leases;
                while (leases.size() < leaseCount)
                    leases.push_back(executor.makeTaskRunner());
                leases.clear();

                ASSERT_THAT(executor,
                            SoonMatches(execStatsElementMatcher(
                                reserved + std::min(maxIdleThreads, leaseCount), 0)))
                    << ", reserved=" << reserved              //
                    << ", maxIdleThreads=" << maxIdleThreads  //
                    << ", leaseCount=" << leaseCount;
            }
        }
    }
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
    JoinThread scheduleClient{[&] { runner->schedule([&](Status s) { pf.promise.setFrom(s); }); }};
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
