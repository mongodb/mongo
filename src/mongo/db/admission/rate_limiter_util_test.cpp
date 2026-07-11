// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/rate_limiter.h"
#include "mongo/db/admission/rate_limiter_otel_metrics_recorder.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/tick_source_mock.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::admission {

namespace {

auto makeClientsWithOpCtxs(ServiceContext* svcCtx, size_t numOps) {
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clientsWithOps;
    for (size_t i = 0; i < numOps; i++) {
        auto client = svcCtx->getService()->makeClient(fmt::format("test client {}", i));
        auto opCtx = client->makeOperationContext();
        clientsWithOps.emplace_back(std::move(client), std::move(opCtx));
    }

    return clientsWithOps;
}

auto constexpr convertBurstSizeToBurstCapacitySecs(double refreshRate, double burstSize) {
    return burstSize / refreshRate;
}

// Spy clock source used to detect whether sleepUntil is called inside DeferredToken::get().
// Intercepts setAlarm (the hook used by waitForConditionUntil on non-system clocks) to:
//   - record the attempt, and
//   - immediately fire the callback so the wait resolves without real-time blocking.
class PreciseClockSourceSpy : public ClockSourceMock {
public:
    bool sleepWasAttempted() const {
        return _sleepAttempted;
    }

    void setAlarm(Date_t when, unique_function<void()> action) override {
        _sleepAttempted = true;
        // Fire the callback immediately so the caller unblocks without any real-time wait.
        // waitForConditionUntil detects the inline execution and returns timeout right away.
        action();
    }

private:
    bool _sleepAttempted = false;
};

// Test fixture that injects PreciseClockSourceSpy as the ServiceContext's precise clock.
class RateLimiterWithPreciseClockSpyTest : public ServiceContextTest {
public:
    RateLimiterWithPreciseClockSpyTest()
        : ServiceContextTest(_initContext(&_clockSpy, &_tickSource)) {}

    void setUp() override {
        static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource())->reset();
        _tickSource->reset(0);
    }

    RateLimiter makeRateLimiter(std::string name,
                                double refreshRate = 1.0,
                                double burstCapacitySecs = 1.0,
                                int64_t maxQueueDepth = INT_MAX) {
        return RateLimiter(refreshRate, burstCapacitySecs, maxQueueDepth, name, _tickSource);
    }

    void advanceTime(Milliseconds d) {
        static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource())->advance(d);
        _tickSource->advance(d);
    }

    PreciseClockSourceSpy* clockSpy() {
        return _clockSpy;
    }

private:
    // Written by _initContext before ServiceContextTest base is constructed (safe for raw ptrs).
    PreciseClockSourceSpy* _clockSpy;
    TickSourceMock<Milliseconds>* _tickSource;

    static std::unique_ptr<ScopedGlobalServiceContextForTest> _initContext(
        PreciseClockSourceSpy** spyOut, TickSourceMock<Milliseconds>** tickOut) {
        auto spy = std::make_unique<PreciseClockSourceSpy>();
        *spyOut = spy.get();
        auto tick = std::make_unique<TickSourceMock<Milliseconds>>();
        *tickOut = tick.get();
        return std::make_unique<ScopedGlobalServiceContextForTest>(ServiceContext::make(
            std::make_unique<ClockSourceMock>(), std::move(spy), std::move(tick)));
    }

    unittest::MinimumLoggedSeverityGuard logSeverityGuard{logv2::LogComponent::kDefault,
                                                          logv2::LogSeverity::Debug(4)};
};

class RateLimiterWithMockClockTest : public ClockSourceMockServiceContextTest {
public:
    void setUp() override {
        static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource())->reset();
        static_cast<TickSourceMock<Milliseconds>*>(getServiceContext()->getTickSource())->reset(0);
    }

    RateLimiter makeRateLimiter(std::string name,
                                double refreshRate = 1.0,
                                double burstCapacitySecs = 1.0,
                                int maxQueueDepth = INT_MAX) {
        return RateLimiter(refreshRate,
                           burstCapacitySecs,
                           maxQueueDepth,
                           name,
                           getServiceContext()->getTickSource());
    }

    void advanceTime(Milliseconds d) {
        static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource())->advance(d);
        static_cast<TickSourceMock<Milliseconds>*>(getServiceContext()->getTickSource())
            ->advance(d);
    }

private:
    unittest::MinimumLoggedSeverityGuard logSeverityGuard{logv2::LogComponent::kDefault,
                                                          logv2::LogSeverity::Debug(4)};
};

// Spec used to drive the OTel-backed recorder in dual-recorder tests. It mirrors the ingress
// request rate limiter's instrument names so the recorder exercises real, registered metrics.
inline RateLimiterOtelMetricsRecorder::MetricsSpec makeOtelMetricsSpec() {
    using otel::metrics::MetricNames;
    return RateLimiterOtelMetricsRecorder::MetricsSpec{
        .attemptedAdmissions = MetricNames::kIngressRequestRateLimiterAttemptedAdmissions,
        .successfulAdmissions = MetricNames::kIngressRequestRateLimiterSuccessfulAdmissions,
        .rejectedAdmissions = MetricNames::kIngressRequestRateLimiterRejectedAdmissions,
        .exemptedAdmissions = MetricNames::kIngressRequestRateLimiterExemptedAdmissions,
        .addedToQueue = MetricNames::kIngressRequestRateLimiterAddedToQueue,
        .removedFromQueue = MetricNames::kIngressRequestRateLimiterRemovedFromQueue,
        .interruptedInQueue = MetricNames::kIngressRequestRateLimiterInterruptedInQueue,
        .tokensAcquired = MetricNames::kIngressRequestRateLimiterTokensAcquired,
        .currentQueueDepth = MetricNames::kIngressRequestRateLimiterCurrentQueueDepth,
        .totalAvailableTokens = MetricNames::kIngressRequestRateLimiterTotalAvailableTokens,
        .averageTimeQueuedMicros = MetricNames::kIngressRequestRateLimiterAverageTimeQueuedMicros,
        .timeQueuedMicros = MetricNames::kIngressRequestRateLimiterTimeQueuedMicros,
    };
}

// Policy that keeps the RateLimiter's default in-process metrics recorder.
class DefaultRecorderPolicy {
public:
    std::unique_ptr<RateLimiterMetricsRecorder> recorder() {
        return std::make_unique<RateLimiterCounterMetricsRecorder>();
    }
};

// Policy that installs an OTel-backed recorder so the same assertions also exercise the path that
// mirrors values into OTel instruments and reads them back through the recorder's accessors.
class OtelRecorderPolicy {
public:
    std::unique_ptr<RateLimiterMetricsRecorder> recorder() {
        return std::make_unique<RateLimiterOtelMetricsRecorder>(makeOtelMetricsSpec());
    }
};

template <typename RecorderPolicy>
class RateLimiterRecorderTest : public RateLimiterWithMockClockTest {
public:
    // Snapshot of the cumulative counters exposed by a recorder. The OTel-backed recorder reads
    // these values back from process-global OTel instruments, which accumulate across tests that
    // run in the same binary (the in-process counter recorder, by contrast, starts at zero per
    // instance). To stay agnostic to which recorder is in use, tests assert on the delta a single
    // test produced rather than on absolute, leak-prone values. The moving-average queue time is
    // tracked per recorder instance and does not leak, so it is read directly and not captured
    // here.
    struct RecorderStats {
        int64_t addedToQueue;
        int64_t removedFromQueue;
        int64_t interruptedInQueue;
        int64_t rejectedAdmissions;
        int64_t successfulAdmissions;
        int64_t exemptedAdmissions;
        int64_t attemptedAdmissions;
        double tokensAcquired;
    };

    void setUp() override {
        RateLimiterWithMockClockTest::setUp();
    }

    std::unique_ptr<RateLimiterMetricsRecorder> recorder() {
        return _policy.recorder();
    }

    RateLimiter makeRateLimiterWithRecorder(
        std::string name,
        std::unique_ptr<RateLimiterMetricsRecorder> recorder =
            std::make_unique<RateLimiterCounterMetricsRecorder>(),
        double refreshRate = 1.0,
        double burstCapacitySecs = 1.0,
        int maxQueueDepth = INT_MAX) {
        return RateLimiter(refreshRate,
                           burstCapacitySecs,
                           maxQueueDepth,
                           name,
                           RateLimiter::Options{.tickSource = getServiceContext()->getTickSource(),
                                                .metricsRecorder = std::move(recorder)});
    }

    // Reads a recorder's current cumulative counters.
    static RecorderStats snapshot(const RateLimiterMetricsRecorder& s) {
        return RecorderStats{
            .addedToQueue = s.addedToQueue(),
            .removedFromQueue = s.removedFromQueue(),
            .interruptedInQueue = s.interruptedInQueue(),
            .rejectedAdmissions = s.rejectedAdmissions(),
            .successfulAdmissions = s.successfulAdmissions(),
            .exemptedAdmissions = s.exemptedAdmissions(),
            .attemptedAdmissions = s.attemptedAdmissions(),
            .tokensAcquired = s.tokensAcquired(),
        };
    }

    // Convenience overload to snapshot a RateLimiter's recorder.
    static RecorderStats snapshot(const RateLimiter& rateLimiter) {
        return snapshot(rateLimiter.stats());
    }

    // Cumulative counters produced since `baseline` was captured. Each test snapshots the
    // RateLimiter's recorder right after construction and passes that baseline here, so the
    // assertions are agnostic to whether the recorder's counters leak across tests (the OTel-backed
    // recorder reads process-global instruments) or start at zero (the in-process counter
    // recorder).
    static RecorderStats statsDelta(const RateLimiter& rateLimiter, const RecorderStats& baseline) {
        const auto current = snapshot(rateLimiter);
        return RecorderStats{
            .addedToQueue = current.addedToQueue - baseline.addedToQueue,
            .removedFromQueue = current.removedFromQueue - baseline.removedFromQueue,
            .interruptedInQueue = current.interruptedInQueue - baseline.interruptedInQueue,
            .rejectedAdmissions = current.rejectedAdmissions - baseline.rejectedAdmissions,
            .successfulAdmissions = current.successfulAdmissions - baseline.successfulAdmissions,
            .exemptedAdmissions = current.exemptedAdmissions - baseline.exemptedAdmissions,
            .attemptedAdmissions = current.attemptedAdmissions - baseline.attemptedAdmissions,
            .tokensAcquired = current.tokensAcquired - baseline.tokensAcquired,
        };
    }

private:
    RecorderPolicy _policy;
};

using RecorderPolicies = ::testing::Types<DefaultRecorderPolicy, OtelRecorderPolicy>;
TYPED_TEST_SUITE(RateLimiterRecorderTest, RecorderPolicies);

// Verify that a RateLimiter with sufficient capacity will dispense a token. Run against every
// metrics recorder implementation so they all report identical accounting.
TYPED_TEST(RateLimiterRecorderTest, BasicTokenAcquisition) {
    RateLimiter rateLimiter =
        this->makeRateLimiterWithRecorder("BasicTokenAcquisition", this->recorder());
    const auto baseline = this->snapshot(rateLimiter);
    auto opCtx = this->makeOperationContext();
    ASSERT_OK(rateLimiter.acquireToken(opCtx.get()));

    const auto stats = this->statsDelta(rateLimiter, baseline);
    ASSERT_EQ(stats.successfulAdmissions, 1);
    ASSERT_EQ(stats.attemptedAdmissions, 1);

    ASSERT_EQ(stats.addedToQueue, 0);
    ASSERT_EQ(stats.removedFromQueue, 0);
    ASSERT_EQ(stats.rejectedAdmissions, 0);
    // Immediate acquisition (without throttling, i.e. no sleep) still counts as a queue time of
    // zero, so the average timing metric will have that as its first value.
    ASSERT(rateLimiter.stats().averageTimeQueuedMicros().has_value());
    ASSERT_APPROX_EQUAL(*rateLimiter.stats().averageTimeQueuedMicros(), 0.0, 0.1);
}

// Verify that RateLimiter::setBurstSize range checks its input.
TEST_F(RateLimiterWithMockClockTest, InvalidBurstSize) {
    ASSERT_THROWS_CODE(
        RateLimiter(1.0, 0, 0, "InvalidBurstSize"), DBException, ErrorCodes::InvalidOptions);

    RateLimiter rateLimiter = makeRateLimiter("InvalidBurstSize");
    ASSERT_THROWS_CODE(
        rateLimiter.updateRateParameters(1.0, 0), DBException, ErrorCodes::InvalidOptions);
}

// Verify that RateLimiter will reject a request for a token if:
// - the request would otherwise be enqueued (there are insufficent tokens),
// - but there are already the maximum number of threads enqueued.
TEST_F(RateLimiterWithMockClockTest, RejectOverMaxWaiters) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr double burstSize = 1.0;
        constexpr double refreshRate = 1.0;
        constexpr double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, burstSize);

        RateLimiter rateLimiter =
            makeRateLimiter("RejectOverMaxWaiters", refreshRate, burstCapacitySecs, 1);
        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), 3);
        Notification<void> firstTokenAcquired;
        Notification<void> hasFailed;
        Status status1 = Status::OK();
        Status status2 = Status::OK();
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // Enqueue two requests, both of which will queue because the mock clock hasn't advanced.
        // Whichever request comes in second will be rejected, because maxQueueDepth is 1.
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            status1 = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            if (!hasFailed) {
                hasFailed.set();
            }
        }));
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            status2 = rateLimiter.acquireToken(clientsWithOps[2].second.get());
            if (!hasFailed) {
                hasFailed.set();
            }
        }));

        // Wait until one of the operations has failed.
        hasFailed.get();

        LOGV2(10574302,
              "At least one thread finished",
              "status1"_attr = status1,
              "status2"_attr = status2);

        // Assert that exactly one of the statuses is from rate limiter rejection.
        ASSERT((status1.code() == RateLimiter::kRejectedErrorCode) ^
               (status2.code() == RateLimiter::kRejectedErrorCode));
        ASSERT_EQ(rateLimiter.stats().rejectedAdmissions(), 1);

        // One token is "borrowed" from the bucket due to the queued request.
        ASSERT_EQ(rateLimiter.tokenBalance(), -1);

        // Interrupt the other token acquisition.
        getServiceContext()->setKillAllOperations();
    });
}

// Verify that if the maximum queue depth is configured to be zero, then any requests that would
// otherwise queue are instead rejected.
TEST_F(RateLimiterWithMockClockTest, QueueingDisabled) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr double burstSize = 1.0;
        constexpr double refreshRate = 1.0;
        constexpr double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, burstSize);

        RateLimiter rateLimiter =
            makeRateLimiter("QueueingDisabled", refreshRate, burstCapacitySecs, 0);
        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), 2);
        Notification<void> firstTokenAcquired;

        std::vector<unittest::JoinThread> threads;
        // Expect the first token acquisition to succeed. We have at least as many tokens
        // available as the configured burst size, which is 1.
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // The next token acquisition attempt fails because it would need to queue.
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            LOGV2(10574301, "Final token acquisition result", "status"_attr = token);
            ASSERT_EQ(token, Status(RateLimiter::kRejectedErrorCode, ""));

            ASSERT_EQ(rateLimiter.stats().successfulAdmissions(), 1);
            ASSERT_EQ(rateLimiter.stats().rejectedAdmissions(), 1);
            ASSERT_EQ(rateLimiter.stats().attemptedAdmissions(), 2);

            // Assert that the token balance is 0, as only one token should have been
            // consumed from the bucket.
            ASSERT_EQ(rateLimiter.tokenBalance(), 0);
        }));
    });
}

// Verify that a negative max queue depth takes the try-acquire path: requests that would queue are
// rejected immediately.
TYPED_TEST(RateLimiterRecorderTest, NegativeMaxQueueDepthDisablesQueueing) {
    RateLimiter rateLimiter =
        this->makeRateLimiterWithRecorder("NegativeMaxQueueDepthDisablesQueueing",
                                          this->recorder(),
                                          /*refreshRate=*/1.0,
                                          /*burstCapacitySecs=*/1.0,
                                          /*maxQueueDepth=*/-1);
    const auto baseline = this->snapshot(rateLimiter);
    auto opCtx = this->makeOperationContext();

    // Consume the initial burst token so the next request must queue.
    ASSERT_OK(rateLimiter.acquireToken(opCtx.get()));

    auto tokenResult = rateLimiter.acquireToken();
    ASSERT_FALSE(tokenResult);
    const auto stats = this->statsDelta(rateLimiter, baseline);
    ASSERT_EQ(stats.addedToQueue, 0);
    ASSERT_EQ(stats.rejectedAdmissions, 1);
    ASSERT_EQ(rateLimiter.queued(), 0);
}

// Verify that if a client disconnects while their session thread is asleep in the rate limiter,
// the rate limiter wakes up the thread and returns the appropriate error status.
TEST_F(RateLimiterWithMockClockTest, InterruptedDueToOperationKilled) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr double burstSize = 1.0;
        constexpr double refreshRate = 1.0;
        constexpr double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, burstSize);

        RateLimiter rateLimiter =
            makeRateLimiter("InterruptedDueToOperationKilled", refreshRate, burstCapacitySecs);
        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), 2);
        Notification<void> firstTokenAcquired;
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // The second token acquisition queues until its opCtx is killed below.
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            ASSERT_EQ(token, Status(ErrorCodes::ClientDisconnect, ""));

            // The first thread was immediately admitted.
            // The second thread was enqueued, then was interrupted, and then was dequeued.
            // The second thread was enqueued for some finite time, so the average queue time
            // could be greater than zero.
            ASSERT_EQ(rateLimiter.stats().successfulAdmissions(), 1);
            ASSERT_EQ(rateLimiter.stats().interruptedInQueue(), 1);
            ASSERT_EQ(rateLimiter.stats().attemptedAdmissions(), 2);
            ASSERT_EQ(rateLimiter.stats().addedToQueue(), 1);
            ASSERT_EQ(rateLimiter.stats().removedFromQueue(), 1);
            ASSERT_NE(rateLimiter.stats().averageTimeQueuedMicros(), boost::none);
            ASSERT_GTE(*rateLimiter.stats().averageTimeQueuedMicros(), 0.0);
            // Assert that the token balance is 0, as only one token should have been
            // consumed from the bucket.
            ASSERT_EQ(rateLimiter.tokenBalance(), 0);
        }));

        firstTokenAcquired.get();
        clientsWithOps[1].second->markKilled(ErrorCodes::ClientDisconnect);
    });
}

// This is like the previous two tests, but instead of a client disconnect or a timeout, it's a
// service shutdown.
TEST_F(RateLimiterWithMockClockTest, InterruptedDueToKillAllOperations) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr double burstSize = 1.0;
        constexpr double refreshRate = 1.0;
        constexpr double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, burstSize);

        RateLimiter rateLimiter = makeRateLimiter(
            "InterruptedDueToKillAllOperations", refreshRate, burstCapacitySecs, INT_MAX);
        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), 2);
        Notification<void> firstTokenAcquired;
        Notification<void> secondTokenInterrupted;
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // The second token acquisition queues until its opCtx is killed.
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            ASSERT_EQ(token, Status(ErrorCodes::InterruptedAtShutdown, ""));
            secondTokenInterrupted.set();
        }));

        firstTokenAcquired.get();
        getServiceContext()->setKillAllOperations();

        // Acquiring a token after shutdown also fails.
        auto client = getServiceContext()->getService()->makeClient("test client");
        auto opCtx = client->makeOperationContext();
        Status token = rateLimiter.acquireToken(opCtx.get());
        ASSERT_EQ(token, Status(ErrorCodes::InterruptedAtShutdown, ""));

        secondTokenInterrupted.get();

        // The first thread was immediately admitted.
        // The second thread was enqueued, then was interrupted by shutdown, and then was
        // dequeued.
        // This thread was enqueued, then was interrupted by shutdown immediately, and then was
        // dequeued.
        // The second thread and this thread were enqueued for some finite time, so the average
        // queue time is not null.
        ASSERT_EQ(rateLimiter.stats().successfulAdmissions(), 1);
        ASSERT_EQ(rateLimiter.stats().interruptedInQueue(), 2);
        ASSERT_EQ(rateLimiter.stats().addedToQueue(), 2);
        ASSERT_EQ(rateLimiter.stats().removedFromQueue(), 2);
        ASSERT_EQ(rateLimiter.stats().attemptedAdmissions(), 3);
        ASSERT_NE(rateLimiter.stats().averageTimeQueuedMicros(), boost::none);
    });
}

// Verify that DeferredToken::get() skips sleepUntil when the clock is advanced past the token's
// ready time before get() is called.
TEST_F(RateLimiterWithPreciseClockSpyTest,
       DeferredTokenGetCompletesAfterClockAdvancedPastReadyTime) {
    const int refreshRate = 4;
    const Milliseconds tokenInterval = Milliseconds(1000) / refreshRate;
    const double burstCapacitySecs = convertBurstSizeToBurstCapacitySecs(refreshRate, 1);

    RateLimiter rateLimiter =
        makeRateLimiter("DeferredTokenGetCompletesAfterClockAdvancedPastReadyTime",
                        refreshRate,
                        burstCapacitySecs,
                        INT_MAX);
    auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), 1);

    // Consume the burst token so the next request must queue (napTime > 0).
    ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));

    // Acquire a deferred token: non-ready, positive napTime.
    auto deferredResult = rateLimiter.acquireToken();
    ASSERT_TRUE(deferredResult);
    auto& deferred = *deferredResult;
    ASSERT_FALSE(deferred.isReady());

    // Advance both tick source and clock past the token's ready time. This simulates the race
    // where time elapsed between enqueue() in acquireToken() and the caller invoking get().
    advanceTime(tokenInterval * 3);

    // get() should return synchronously without calling sleepUntil because adjustedNapTime == 0.
    ASSERT_OK(std::move(deferred).get(clientsWithOps[0].second.get()));

    ASSERT_FALSE(clockSpy()->sleepWasAttempted())
        << "get() should not call sleepUntil when the token ready time has already passed";
    ASSERT_EQ(rateLimiter.stats().successfulAdmissions(), 2);
    ASSERT_EQ(rateLimiter.stats().addedToQueue(), 1);
    ASSERT_EQ(rateLimiter.stats().removedFromQueue(), 1);
}

// Verify that `RateLimiter::recordExemption()` increments the exemption metric but no others.
TYPED_TEST(RateLimiterRecorderTest, RecordExemption) {
    RateLimiter rateLimiter =
        this->makeRateLimiterWithRecorder("RecordExemption", this->recorder());
    const auto baseline = this->snapshot(rateLimiter);
    rateLimiter.recordExemption();

    const auto stats = this->statsDelta(rateLimiter, baseline);
    ASSERT_EQ(stats.exemptedAdmissions, 1);

    ASSERT_EQ(stats.attemptedAdmissions, 0);
    ASSERT_EQ(stats.successfulAdmissions, 0);
    ASSERT_EQ(stats.interruptedInQueue, 0);
    ASSERT_EQ(stats.addedToQueue, 0);
    ASSERT_EQ(stats.removedFromQueue, 0);
    ASSERT_EQ(rateLimiter.stats().averageTimeQueuedMicros(), boost::none);
}

// Verify that a newly initialized RateLimiter can immediately dispense up to its burst rate of
// tokens, and thereafter releases tokens at its configured rate.
// If multiple threads concurrently request tokens, then some of the threads will be admitted
// immediately, while the remainder will be admitted as tokens become available.
TEST_F(RateLimiterWithMockClockTest, ConcurrentTokenAcquisitionWithQueueing) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        const int maxTokens = 2;
        const int refreshRate = 4;
        const double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, maxTokens);
        const int64_t numThreads = 10;
        const Milliseconds tokenInterval = Milliseconds(1000) / refreshRate;

        RateLimiter rateLimiter = makeRateLimiter(
            "ConcurrentTokenAcquisitionWithQueueing", refreshRate, burstCapacitySecs, INT_MAX);

        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), numThreads);

        std::mutex mutex;
        stdx::condition_variable cv;

        std::vector<double> tokenAcquisitionTimes;

        std::vector<unittest::JoinThread> threads;
        for (int64_t i = 0; i < numThreads; i++) {
            threads.emplace_back(monitor.spawn([&, threadNum = i]() {
                ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[threadNum].second.get()));
                std::lock_guard lg(mutex);
                LOGV2(10440801,
                      "Acquired token",
                      "threadNum"_attr = threadNum,
                      "mockClockNow"_attr =
                          getServiceContext()->getFastClockSource()->now().toMillisSinceEpoch());
                tokenAcquisitionTimes.emplace_back(
                    getServiceContext()->getFastClockSource()->now().toMillisSinceEpoch());
                cv.notify_one();
            }));
        }

        // Make sure the initial burstRate fulfills the first requests
        {
            std::unique_lock<std::mutex> lk(mutex);
            ASSERT_DOES_NOT_THROW(
                cv.wait(lk, [&] { return (int)tokenAcquisitionTimes.size() == maxTokens; }));
        }

        // At this point, maxTokens threads have been allowed through. The rest of the threads
        // are about to be blocked in sleep (or have been already).
        // Queue timing metric samples have been collected, but they're also zero due to the
        // burst availability.
        // Some threads might have been enqueued, but none have been dequeued.
        ASSERT_EQ(rateLimiter.stats().successfulAdmissions(), maxTokens);
        ASSERT_EQ(rateLimiter.stats().removedFromQueue(), 0);
        ASSERT_EQ(rateLimiter.stats().rejectedAdmissions(), 0);
        ASSERT(rateLimiter.stats().averageTimeQueuedMicros().has_value());
        ASSERT_APPROX_EQUAL(*rateLimiter.stats().averageTimeQueuedMicros(), 0.0, .1);

        // Make sure we've enqueued all the remaining waiters so that we don't race with advancing
        // the mock clock. Use a real-time deadline instead of a bounded retry loop so the wait
        // scales correctly under slow sanitizer builds (e.g. AUBSAN ~10x slower).
        const auto enqueueDeadline = Date_t::now() + Seconds(60);
        while (rateLimiter.queued() != numThreads - maxTokens) {
            if (Date_t::now() >= enqueueDeadline) {
                break;
            }
            sleepmillis(5);
        }

        // Until we start moving the mock clock forward, no other requests will be fulfilled and all
        // other requests will be waiting.
        ASSERT_EQ(rateLimiter.queued(), numThreads - maxTokens);
        ASSERT_EQ((int)tokenAcquisitionTimes.size(), maxTokens);

        ASSERT_EQ(rateLimiter.stats().successfulAdmissions(), maxTokens);
        ASSERT_EQ(rateLimiter.stats().addedToQueue(), numThreads - maxTokens);
        ASSERT_EQ(rateLimiter.stats().removedFromQueue(), 0);
        ASSERT_EQ(rateLimiter.stats().rejectedAdmissions(), 0);
        ASSERT(rateLimiter.stats().averageTimeQueuedMicros().has_value());
        ASSERT_APPROX_EQUAL(*rateLimiter.stats().averageTimeQueuedMicros(), 0.0, .1);

        // Advancing time less than tokenInterval doesn't cause a token to be acquired.
        auto smallAdvance = Milliseconds{10};
        ASSERT_LT(smallAdvance, tokenInterval);
        advanceTime(smallAdvance);
        ASSERT_EQ((int)tokenAcquisitionTimes.size(), maxTokens);

        // Advance time by enough for all remaining tokens, with some extra buffer to ensure all
        // remaining threads can acquire tokens after this time advance. Each token needs
        // tokenInterval, so advance by (remainingTokens + buffer) * tokenInterval.
        const auto deadline = (Date_t::now() + Seconds(30)).toSystemTimePoint();
        const int64_t remainingTokens = numThreads - maxTokens;
        Milliseconds totalAdvance = tokenInterval * (remainingTokens + 2);
        advanceTime(totalAdvance);

        {
            std::unique_lock<std::mutex> lk(mutex);
            // Wait for all threads to acquire tokens.
            if (!cv.wait_until(lk, deadline, [&] {
                    return (int)tokenAcquisitionTimes.size() == numThreads;
                })) {
                getServiceContext()->setKillAllOperations();
                // We re-run this assertion so that proper diagnostics are output.
                ASSERT_EQ((int)tokenAcquisitionTimes.size(), numThreads);
            }
        }

        // Verify final metrics.
        ASSERT_EQ(rateLimiter.stats().successfulAdmissions(), numThreads);
        ASSERT_EQ(rateLimiter.stats().removedFromQueue(), remainingTokens);
        ASSERT_NE(rateLimiter.stats().averageTimeQueuedMicros(), boost::none);
        ASSERT_GTE(*rateLimiter.stats().averageTimeQueuedMicros(), 0.0);

        ASSERT_EQ((int)tokenAcquisitionTimes.size(), numThreads);

        // Assert that the initial burst tokens were acquired immediately (at time 1).
        for (int64_t i = 0; i < maxTokens; i++) {
            ASSERT_APPROX_EQUAL(tokenAcquisitionTimes[i], 1, 1e-3);
        }

        // For remaining tokens, verify they were acquired after the burst.
        // Due to the naptime race, we can't assert exact timing, but we can verify:
        // 1. All were acquired after the initial burst
        // 2. They were acquired after smallAdvance (the first sub-tokenInterval advance)
        double minExpectedTime = 1 + durationCount<Milliseconds>(smallAdvance);
        for (int64_t i = maxTokens; i < numThreads; i++) {
            ASSERT_GTE(tokenAcquisitionTimes[i], minExpectedTime);
        }

        // By the end, there was one attempt per thread.
        ASSERT_EQ(rateLimiter.stats().attemptedAdmissions(), numThreads);
    });
}

TEST_F(RateLimiterWithMockClockTest, TokenBalanceIsValidAfterRejectedRequests) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr double burstSize = 1.0;
        constexpr double refreshRate = 1.0;
        constexpr int maxQueueDepth = 0;
        constexpr double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, burstSize);

        RateLimiter rateLimiter = makeRateLimiter(
            "RateLimitIsValidAfterRejectedRequests", refreshRate, burstCapacitySecs, maxQueueDepth);
        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), 2);
        Notification<void> firstTokenAcquired;
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // The second token acquisition is rejected and there is no net effect on the token bucket
        // balance.
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            ASSERT_EQ(rateLimiter.tokenBalance(), 0);
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            ASSERT_EQ(token, Status(RateLimiter::kRejectedErrorCode, ""));
            ASSERT_EQ(rateLimiter.tokenBalance(), 0);
        }));

        firstTokenAcquired.get();
    });
}

// Verify that if the sleep within an enqueued thread is interrupted, then the rate limiter
// returns the error status corresponding to the reason for the interruption.
TEST_F(RateLimiterWithMockClockTest, InterruptedDueToOperationDeadline) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr double burstSize = 1.0;
        constexpr double refreshRate = 1.0;
        constexpr double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, burstSize);

        RateLimiter rateLimiter = makeRateLimiter(
            "InterruptedDueToOperationDeadline", refreshRate, burstCapacitySecs, INT_MAX);
        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), 2);
        Notification<void> firstTokenAcquired;
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // The second token acquisition queues until its opCtx deadline passes and there is no net
        // effect on the token bucket balance after it times out.
        clientsWithOps[1].second->setDeadlineByDate(
            getServiceContext()->getFastClockSource()->now() + Milliseconds(5),
            ErrorCodes::MaxTimeMSExpired);
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            ASSERT_APPROX_EQUAL(rateLimiter.tokenBalance(), 0, .5);
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            ASSERT_EQ(token, Status(ErrorCodes::MaxTimeMSExpired, ""));
            ASSERT_APPROX_EQUAL(rateLimiter.tokenBalance(), 0, .5);

            // The first thread was immediately admitted.
            // The second thread was enqueued, then timed out, and then was dequeued.
            // The second thread was enqueued for some finite time, so the average queue time
            // could be greater than zero.
            ASSERT_EQ(rateLimiter.stats().successfulAdmissions(), 1);
            ASSERT_EQ(rateLimiter.stats().interruptedInQueue(), 1);
            ASSERT_EQ(rateLimiter.stats().addedToQueue(), 1);
            ASSERT_EQ(rateLimiter.stats().attemptedAdmissions(), 2);
            ASSERT_EQ(rateLimiter.stats().removedFromQueue(), 1);
            ASSERT_NE(rateLimiter.stats().averageTimeQueuedMicros(), boost::none);
            ASSERT_GTE(*rateLimiter.stats().averageTimeQueuedMicros(), 0.0);
        }));

        firstTokenAcquired.get();
        advanceTime(Milliseconds(5));
    });
}

// Verify that calling recordExemption() on a non-ready DeferredToken records the exemption stat,
// and that DeferredToken destruction then returns the borrowed token and releases the queue slot.
TYPED_TEST(RateLimiterRecorderTest, DeferredTokenRecordExemptionAndReleaseQueueSlot) {
    RateLimiter rateLimiter = this->makeRateLimiterWithRecorder(
        "DeferredTokenRecordExemptionAndReleaseQueueSlot", this->recorder());
    const auto baseline = this->snapshot(rateLimiter);
    auto opCtx = this->makeOperationContext();

    // Exhaust the burst so the next token must queue.
    ASSERT_OK(rateLimiter.acquireToken(opCtx.get()));
    ASSERT_EQ(rateLimiter.tokenBalance(), 0);

    {
        auto tokenResult = rateLimiter.acquireToken();
        ASSERT_TRUE(tokenResult);
        auto deferredToken = std::move(*tokenResult);
        ASSERT_FALSE(deferredToken.isReady());
        // Token is borrowed (bucket goes negative).
        ASSERT_EQ(rateLimiter.tokenBalance(), -1);
        ASSERT_EQ(rateLimiter.queued(), 1);
        ASSERT_EQ(this->statsDelta(rateLimiter, baseline).addedToQueue, 1);

        // Mark as exempt: records the stat, queued-token cleanup runs in the DeferredToken
        // destructor.
        std::move(deferredToken).recordExemption();
        ASSERT_EQ(this->statsDelta(rateLimiter, baseline).exemptedAdmissions, 1);
    }

    ASSERT_EQ(rateLimiter.tokenBalance(), 0);
    ASSERT_EQ(rateLimiter.queued(), 0);
    ASSERT_EQ(this->statsDelta(rateLimiter, baseline).removedFromQueue, 1);
    // The exempted DeferredToken was never admitted successfully.
    ASSERT_EQ(this->statsDelta(rateLimiter, baseline).successfulAdmissions, 1);
}

// Verify that dropping a non-ready DeferredToken without consuming it causes the destructor
// to return the borrowed token and release the queue slot, preventing resource leaks.
TYPED_TEST(RateLimiterRecorderTest, DeferredTokenDestructorCleansUpDroppedNonReadyToken) {
    RateLimiter rateLimiter = this->makeRateLimiterWithRecorder(
        "DeferredTokenDestructorCleansUpDroppedNonReadyToken", this->recorder());
    const auto baseline = this->snapshot(rateLimiter);
    auto opCtx = this->makeOperationContext();

    ASSERT_OK(rateLimiter.acquireToken(opCtx.get()));
    ASSERT_EQ(rateLimiter.tokenBalance(), 0);

    {
        auto tokenResult = rateLimiter.acquireToken();
        ASSERT_TRUE(tokenResult);
        auto deferredToken = std::move(*tokenResult);
        ASSERT_FALSE(deferredToken.isReady());
        ASSERT_EQ(rateLimiter.tokenBalance(), -1);
        ASSERT_EQ(rateLimiter.queued(), 1);
        // Drop DeferredToken without calling get() or recordExemption().
    }

    ASSERT_EQ(rateLimiter.tokenBalance(), 0);
    ASSERT_EQ(rateLimiter.queued(), 0);
    const auto stats = this->statsDelta(rateLimiter, baseline);
    ASSERT_EQ(stats.addedToQueue, 1);
    ASSERT_EQ(stats.removedFromQueue, 1);
    ASSERT_EQ(stats.successfulAdmissions, 1);
}

// Verify that dropping a ready DeferredToken without calling get() is a no-op: the token was
// already
// permanently consumed by acquireToken() and successfulAdmissions was already recorded.
TYPED_TEST(RateLimiterRecorderTest, DeferredTokenReadyDestructorIsNoOp) {
    RateLimiter rateLimiter =
        this->makeRateLimiterWithRecorder("DeferredTokenReadyDestructorIsNoOp", this->recorder());
    const auto baseline = this->snapshot(rateLimiter);
    const double initialBalance = rateLimiter.tokenBalance();

    {
        auto tokenResult = rateLimiter.acquireToken();
        ASSERT_TRUE(tokenResult);
        auto deferredToken = std::move(*tokenResult);
        ASSERT_TRUE(deferredToken.isReady());
        // For ready DeferredTokens, acquireToken() already incremented successfulAdmissions.
        ASSERT_EQ(this->statsDelta(rateLimiter, baseline).successfulAdmissions, 1);
        ASSERT_EQ(rateLimiter.tokenBalance(), initialBalance - 1);
        // Destructor runs here — should be a no-op for ready DeferredTokens.
    }

    // Token remains consumed; no return to bucket.
    ASSERT_EQ(rateLimiter.tokenBalance(), initialBalance - 1);
    const auto stats = this->statsDelta(rateLimiter, baseline);
    ASSERT_EQ(stats.addedToQueue, 0);
    ASSERT_EQ(stats.removedFromQueue, 0);
}

// Verify that moving a DeferredToken nulls the source (making its destructor a no-op) and
// that exactly one cleanup occurs when the destination goes out of scope.
TYPED_TEST(RateLimiterRecorderTest, DeferredTokenMoveSemantics) {
    RateLimiter rateLimiter =
        this->makeRateLimiterWithRecorder("DeferredTokenMoveSemantics", this->recorder());
    const auto baseline = this->snapshot(rateLimiter);
    auto opCtx = this->makeOperationContext();

    // Exhaust burst so the next token queues.
    ASSERT_OK(rateLimiter.acquireToken(opCtx.get()));

    auto tokenResult = rateLimiter.acquireToken();
    ASSERT_TRUE(tokenResult);

    {
        auto deferredToken1 = std::move(*tokenResult);
        ASSERT_FALSE(deferredToken1.isReady());
        ASSERT_EQ(rateLimiter.queued(), 1);

        // Move-construct into deferredToken2.
        auto deferredToken2 = std::move(deferredToken1);
        ASSERT_FALSE(deferredToken2.isReady());
        // Queue count is unchanged; the move transferred ownership, not the slot.
        ASSERT_EQ(rateLimiter.queued(), 1);

        // deferredToken1 destructs (moved-from, _impl is null — no-op).
        // deferredToken2 destructs (active — returns token and releases queue slot).
    }

    ASSERT_EQ(rateLimiter.queued(), 0);
    ASSERT_EQ(rateLimiter.tokenBalance(), 0);
    const auto stats = this->statsDelta(rateLimiter, baseline);
    ASSERT_EQ(stats.addedToQueue, 1);
    ASSERT_EQ(stats.removedFromQueue, 1);
}

TYPED_TEST(RateLimiterRecorderTest, TokensAcquiredMetric) {
    RateLimiter rateLimiter = this->makeRateLimiterWithRecorder("TokensAcquiredMetric",
                                                                this->recorder(),
                                                                /*refreshRate=*/100.0,
                                                                /*burstCapacitySecs=*/3.0);
    const auto baseline = this->snapshot(rateLimiter);
    auto opCtx = this->makeOperationContext();

    ASSERT_OK(rateLimiter.acquireToken(opCtx.get(), 1.0));
    ASSERT_EQ(this->statsDelta(rateLimiter, baseline).tokensAcquired, 1.0);

    ASSERT_OK(rateLimiter.acquireToken(opCtx.get(), 2.0));
    ASSERT_EQ(this->statsDelta(rateLimiter, baseline).tokensAcquired, 3.0);
}

TEST_F(RateLimiterWithMockClockTest, ReturnTokens) {
    RateLimiter rateLimiter = makeRateLimiter("RateLimiterReturnTokens",
                                              /*refreshRate=*/100,
                                              /*burstCapacitySecs=*/3,
                                              /*maxQueueDepth=*/100);
    auto opCtx = makeOperationContext();
    auto maxTokens = 100 * 3;  // refreshRate * getBurstCapacitySecs
    auto tokensToConsume = 100;
    ASSERT_EQ(rateLimiter.tokenBalance(), maxTokens);

    ASSERT_OK(rateLimiter.acquireToken(opCtx.get(), tokensToConsume));

    // Assert we should have 300 - 100 tokens as a balance here.
    ASSERT_EQ(rateLimiter.tokenBalance(), maxTokens - tokensToConsume);

    // Return some tokens
    auto tokensToReturn = 50;
    rateLimiter.returnTokens(tokensToReturn);

    // Assert that calling returnTokens returns the expected number of tokens to the bucket.
    ASSERT_EQ(rateLimiter.tokenBalance(), maxTokens - tokensToConsume + tokensToReturn);
}

TEST_F(RateLimiterWithMockClockTest, ReconcileTokens) {
    RateLimiter rateLimiter = makeRateLimiter("RateLimiterReconcileTokens",
                                              /*refreshRate=*/100,
                                              /*burstCapacitySecs=*/3,
                                              /*maxQueueDepth=*/100);
    const auto maxTokens = 100 * 3;  // refreshRate * burstCapacitySecs
    ASSERT_EQ(rateLimiter.tokenBalance(), maxTokens);

    // Non-positive reconciliations are no-ops.
    rateLimiter.reconcileTokens(0);
    rateLimiter.reconcileTokens(-5);
    ASSERT_EQ(rateLimiter.tokenBalance(), maxTokens);

    // Reconciliation drains the bucket without blocking.
    rateLimiter.reconcileTokens(50);
    ASSERT_EQ(rateLimiter.tokenBalance(), maxTokens - 50);

    // Reconciling past the available balance borrows (the balance goes negative).
    rateLimiter.reconcileTokens(maxTokens);
    ASSERT_EQ(rateLimiter.tokenBalance(), -50);

    // Reconciliation adjusts only the balance; it records no admission.
    ASSERT_EQ(rateLimiter.stats().successfulAdmissions(), 0);
    ASSERT_EQ(rateLimiter.stats().attemptedAdmissions(), 0);
}

}  // namespace
}  // namespace mongo::admission
