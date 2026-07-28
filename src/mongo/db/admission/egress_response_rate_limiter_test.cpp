// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/egress_response_rate_limiter.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/join_thread.h"
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
        int64_t addedToQueue;
        int64_t removedFromQueue;
        int64_t interruptedInQueue;
    };

    StatsSnapshot snapshotStats() {
        auto& s = EgressResponseRateLimiter::get(getServiceContext()).stats();
        return {s.successfulAdmissions(),
                s.addedToQueue(),
                s.removedFromQueue(),
                s.interruptedInQueue()};
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

}  // namespace
}  // namespace mongo::admission
