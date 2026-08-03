// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/egress_response_rate_limiter.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::admission {
namespace {

class EgressResponseRateLimiterTest : public ClockSourceMockServiceContextTest {
public:
    // The egress limiter's counters are backed by process-global OTel instruments (see
    // RateLimiterOtelMetricsRecorder), so they accumulate across tests. Snapshot a baseline and
    // assert on the delta each test produces, mirroring the IngressRequestRateLimiter tests.
    struct StatsSnapshot {
        int64_t successfulAdmissions;
        int64_t rejectedAdmissions;
        int64_t attemptedAdmissions;
        int64_t addedToQueue;
        int64_t removedFromQueue;
        int64_t interruptedInQueue;
    };

    StatsSnapshot snapshotStats() {
        auto& s = EgressResponseRateLimiter::get(getServiceContext()).stats();
        return {s.successfulAdmissions(),
                s.rejectedAdmissions(),
                s.attemptedAdmissions(),
                s.addedToQueue(),
                s.removedFromQueue(),
                s.interruptedInQueue()};
    }

    static int64_t currentQueueDepthDelta(const StatsSnapshot& baseline, const StatsSnapshot& now) {
        return (now.addedToQueue - baseline.addedToQueue) -
            (now.removedFromQueue - baseline.removedFromQueue);
    }
};

// With default parameters (rate = max), throttle() admits immediately without queueing.
TEST_F(EgressResponseRateLimiterTest, DefaultIsNoOpAndAdmitsImmediately) {
    auto& limiter = EgressResponseRateLimiter::get(getServiceContext());
    const auto baseline = snapshotStats();

    // Default rate is max, so throttle() admits immediately without queueing; the Interruptible is
    // never parked, so the non-interruptible default suffices.
    ASSERT_OK(limiter.throttle(Interruptible::notInterruptible(),
                               getServiceContext()->getPreciseClockSource()));

    auto& stats = limiter.stats();
    ASSERT_EQ(stats.successfulAdmissions(), baseline.successfulAdmissions + 1);
    ASSERT_EQ(stats.addedToQueue(), baseline.addedToQueue);
    ASSERT_EQ(stats.removedFromQueue(), baseline.removedFromQueue);
}

// onUpdateRatePerSec reconfigures the live limiter; refreshRate() reflects the new rate.
TEST_F(EgressResponseRateLimiterTest, OnUpdateRatePerSecReconfiguresLimiter) {
    auto& limiter = EgressResponseRateLimiter::get(getServiceContext());
    ASSERT_OK(EgressResponseRateLimiter::onUpdateRatePerSec(123));
    ASSERT_EQ(limiter.refreshRate(), 123);
}

TEST_F(EgressResponseRateLimiterTest, OnUpdateBurstCapacitySecsReconfiguresLimiter) {
    auto& limiter = EgressResponseRateLimiter::get(getServiceContext());
    ASSERT_OK(EgressResponseRateLimiter::onUpdateBurstCapacitySecs(2.0));
    // refreshRate() is unchanged (still the default max); only burst changed. Re-assert rate is
    // unaffected and the call is accepted.
    ASSERT_GT(limiter.refreshRate(), 0.0);
}

// A queued egress wait is interrupted when its opCtx is killed: throttle() returns Interrupted,
// the borrowed token is returned, and interruptedInQueue increments. This covers the egress
// throttle() wrapper's interruptibility through a real Interruptible (an OperationContext). The
// shutdown-cancellable Interruptible used in production (owned by SessionManagerCommon) is tested
// in isolation in the transport layer.
TEST_F(EgressResponseRateLimiterTest, QueuedWaitInterruptedByKilledOpCtx) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr int refreshRate = 4;  // 4 tokens/sec => 250ms per token
        constexpr double burstSize = 1.0;
        constexpr double burstCapacitySecs = burstSize / refreshRate;

        auto& limiter = EgressResponseRateLimiter::get(getServiceContext());
        limiter.updateRateParameters(refreshRate, burstCapacitySecs);

        auto client = getServiceContext()->getService()->makeClient("egress-test");
        auto opCtx = client->makeOperationContext();
        const auto baseline = snapshotStats();

        // Consume the single burst token (admits immediately).
        ASSERT_OK(
            limiter.throttle(opCtx.get(), opCtx->getServiceContext()->getPreciseClockSource()));

        Notification<void> throttleReturned;
        std::vector<unittest::JoinThread> threads;
        threads.emplace_back(monitor.spawn([&]() {
            // This call queues (no token available) and blocks until the opCtx is killed below.
            auto s =
                limiter.throttle(opCtx.get(), opCtx->getServiceContext()->getPreciseClockSource());
            ASSERT_NOT_OK(s);
            ASSERT_EQ(s.code(), ErrorCodes::ClientDisconnect);
            throttleReturned.set();
        }));

        // Wait until the second call has enqueued.
        const auto enqueueDeadline = Date_t::now() + Seconds(30);
        while (limiter.stats().addedToQueue() != baseline.addedToQueue + 1) {
            if (Date_t::now() >= enqueueDeadline) {
                break;
            }
            sleepmillis(1);
        }
        ASSERT_EQ(limiter.stats().addedToQueue(), baseline.addedToQueue + 1);

        // Kill the opCtx as a client disconnect would. The waiter wakes with ClientDisconnect and
        // returns the borrowed token.
        opCtx->markKilled(ErrorCodes::ClientDisconnect);
        throttleReturned.get();

        ASSERT_EQ(limiter.stats().interruptedInQueue(), baseline.interruptedInQueue + 1);
        ASSERT_EQ(limiter.stats().removedFromQueue(), baseline.removedFromQueue + 1);
        ASSERT_EQ(limiter.stats().successfulAdmissions(), baseline.successfulAdmissions + 1);
    });
}

// Setting egressResponseRateLimiterMaxQueueDepth=0 disables queueing: once the burst is exhausted,
// further throttle() calls take the try-acquire path and return immediately. The fail-open
// invariant holds -- throttle() still returns OK (the caller proceeds to send the response),
// nothing is enqueued, and the rejection is recorded.
TEST_F(EgressResponseRateLimiterTest, MaxQueueDepthZeroDisablesQueueingOverflowIsFailOpen) {
    constexpr int refreshRate = 1;             // 1 token/sec
    constexpr double burstCapacitySecs = 1.0;  // burst size = 1 token

    auto& limiter = EgressResponseRateLimiter::get(getServiceContext());
    unittest::ServerParameterGuard maxQueueDepth{"egressResponseRateLimiterMaxQueueDepth",
                                                 static_cast<long long>(0)};
    limiter.updateRateParameters(refreshRate, burstCapacitySecs);

    const auto baseline = snapshotStats();
    auto* clock = getServiceContext()->getPreciseClockSource();

    // First call consumes the single burst token via the try-acquire path (queueing disabled).
    ASSERT_OK(limiter.throttle(Interruptible::notInterruptible(), clock));
    ASSERT_EQ(limiter.stats().successfulAdmissions(), baseline.successfulAdmissions + 1);
    ASSERT_EQ(limiter.stats().addedToQueue(), baseline.addedToQueue);

    // Second call: burst exhausted, queueing disabled. tryAcquire fails, acquireToken returns
    // none, and throttle() returns OK (fail-open) so the response is still sent.
    ASSERT_OK(limiter.throttle(Interruptible::notInterruptible(), clock));
    ASSERT_EQ(limiter.stats().rejectedAdmissions(), baseline.rejectedAdmissions + 1);
    ASSERT_EQ(limiter.stats().attemptedAdmissions(), baseline.attemptedAdmissions + 2);
    ASSERT_EQ(limiter.stats().addedToQueue(), baseline.addedToQueue);
    ASSERT_EQ(currentQueueDepthDelta(baseline, snapshotStats()), 0);
}

// A finite maxQueueDepth bounds the queue. Once at capacity, an overflow throttle() call takes the
// try-acquire path and returns OK without enqueuing (fail-open). The response is still sendable and
// currentQueueDepth never exceeds the configured bound.
TEST_F(EgressResponseRateLimiterTest, FiniteMaxQueueDepthBoundsQueueAndOverflowIsFailOpen) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr int refreshRate = 4;              // 4 tokens/sec => 250ms per token
        constexpr double burstCapacitySecs = 0.25;  // burst size = 1 token
        constexpr int64_t kMaxQueueDepth = 2;

        auto& limiter = EgressResponseRateLimiter::get(getServiceContext());
        limiter.updateMaxQueueDepth(kMaxQueueDepth);
        limiter.updateRateParameters(refreshRate, burstCapacitySecs);

        auto* clock = getServiceContext()->getPreciseClockSource();
        const auto baseline = snapshotStats();

        // Consume the single burst token (admits immediately, no blocking).
        ASSERT_OK(limiter.throttle(Interruptible::notInterruptible(), clock));

        // Each waiter gets its own Client/OperationContext: OperationContext is not safe for
        // concurrent sleepUntil from multiple threads, so no two waiters may share one.
        std::vector<ServiceContext::UniqueClient> waiterClients;
        std::vector<ServiceContext::UniqueOperationContext> waiterOpCtxs;
        for (int i = 0; i < kMaxQueueDepth; ++i) {
            waiterClients.emplace_back(
                getServiceContext()->getService()->makeClient("egress-queue-waiter"));
            waiterOpCtxs.emplace_back(waiterClients.back()->makeOperationContext());
        }

        // Fill the queue to capacity with blocking waiters, each on its own opCtx.
        std::vector<unittest::JoinThread> threads;
        for (int i = 0; i < kMaxQueueDepth; ++i) {
            OperationContext* waiterOpCtx = waiterOpCtxs[i].get();
            threads.emplace_back(monitor.spawn([&, waiterOpCtx]() {
                auto s = limiter.throttle(waiterOpCtx, clock);
                ASSERT_NOT_OK(s);
                ASSERT_EQ(s.code(), ErrorCodes::ClientDisconnect);
            }));
        }

        // Wait until the queue is at capacity.
        const auto enqueueDeadline = Date_t::now() + Seconds(30);
        while (currentQueueDepthDelta(baseline, snapshotStats()) < kMaxQueueDepth) {
            if (Date_t::now() >= enqueueDeadline) {
                break;
            }
            sleepmillis(1);
        }
        ASSERT_EQ(currentQueueDepthDelta(baseline, snapshotStats()), kMaxQueueDepth);

        // Overflow: queue is full. throttle() must be fail-open -- returns OK without enqueuing,
        // so currentQueueDepth stays bounded at kMaxQueueDepth.
        ASSERT_OK(limiter.throttle(Interruptible::notInterruptible(), clock));
        ASSERT_EQ(limiter.stats().addedToQueue(), baseline.addedToQueue + kMaxQueueDepth);
        ASSERT_EQ(currentQueueDepthDelta(baseline, snapshotStats()), kMaxQueueDepth);

        for (auto& waiterOpCtx : waiterOpCtxs) {
            waiterOpCtx->markKilled(ErrorCodes::ClientDisconnect);
        }
        threads.clear();

        ASSERT_EQ(limiter.stats().interruptedInQueue(),
                  baseline.interruptedInQueue + kMaxQueueDepth);
        ASSERT_EQ(limiter.stats().removedFromQueue(), baseline.removedFromQueue + kMaxQueueDepth);
    });
}

}  // namespace
}  // namespace mongo::admission
