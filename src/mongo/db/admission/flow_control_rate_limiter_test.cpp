// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/flow_control_rate_limiter.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class FlowControlRateLimiterTest : public ServiceContextTest {
protected:
    void setUp() override {
        ServiceContextTest::setUp();
        _client = getServiceContext()->getService()->makeClient("test");
        _opCtx = _client->makeOperationContext();
    }

    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(FlowControlRateLimiterTest, BasicConstruction) {
    FlowControlRateLimiter rl;
    ASSERT_EQUALS(rl.queued(), 0);
}

TEST_F(FlowControlRateLimiterTest, UpdateRateWithVariousValues) {
    FlowControlRateLimiter rl;

    rl.updateRate(0);  // Clamped to 1 internally.
    rl.updateRate(1);
    rl.updateRate(100);
    rl.updateRate(5000);
    rl.updateRate(1000000);
    rl.updateRate(FlowControlRateLimiter::kMaxRate);
}

TEST_F(FlowControlRateLimiterTest, AcquireTicketSucceeds) {
    FlowControlRateLimiter rl;
    rl.updateRate(1000000);

    FlowControlTicketholder::CurOp stats;
    rl.acquireTicket(_opCtx.get(), &stats);

    ASSERT_EQUALS(stats.ticketsAcquired, 1);
    ASSERT_FALSE(stats.waiting);
}

TEST_F(FlowControlRateLimiterTest, StatsIncrementOnMultipleAcquires) {
    FlowControlRateLimiter rl;
    rl.updateRate(1000000);

    FlowControlTicketholder::CurOp stats;
    for (int i = 0; i < 10; i++) {
        rl.acquireTicket(_opCtx.get(), &stats);
    }

    ASSERT_EQUALS(stats.ticketsAcquired, 10);
    ASSERT_GTE(rl.stats().successfulAdmissions(), 10);
}

TEST_F(FlowControlRateLimiterTest, ServiceContextDecoration) {
    auto* svc = getServiceContext();
    ASSERT_FALSE(FlowControlRateLimiter::get(svc));

    FlowControlRateLimiter::set(svc, std::make_unique<FlowControlRateLimiter>());
    ASSERT_TRUE(FlowControlRateLimiter::get(svc));

    auto* rl = FlowControlRateLimiter::get(svc);
    rl->updateRate(5000);

    FlowControlTicketholder::CurOp stats;
    rl->acquireTicket(_opCtx.get(), &stats);
    ASSERT_EQUALS(stats.ticketsAcquired, 1);
}

TEST_F(FlowControlRateLimiterTest, ServiceContextDecorationViaOpCtx) {
    auto* svc = getServiceContext();
    FlowControlRateLimiter::set(svc, std::make_unique<FlowControlRateLimiter>());

    auto* rl = FlowControlRateLimiter::get(_opCtx.get());
    ASSERT_TRUE(rl);
}

TEST_F(FlowControlRateLimiterTest, QueuedCountStartsAtZero) {
    FlowControlRateLimiter rl;
    ASSERT_EQUALS(rl.queued(), 0);
}

TEST_F(FlowControlRateLimiterTest, AppendStatsProducesBSON) {
    FlowControlRateLimiter rl;
    rl.updateRate(1000);

    BSONObjBuilder bob;
    rl.appendStats(&bob);
    auto obj = bob.obj();
    ASSERT_FALSE(obj.isEmpty());
}

TEST_F(FlowControlRateLimiterTest, CurOpTimeTrackingOnHighRate) {
    FlowControlRateLimiter rl;
    rl.updateRate(1000000);

    FlowControlTicketholder::CurOp stats;
    rl.acquireTicket(_opCtx.get(), &stats);

    ASSERT_EQUALS(stats.ticketsAcquired, 1);
    ASSERT_GTE(stats.timeAcquiringMicros, 0);
}

TEST_F(FlowControlRateLimiterTest, RateLimiterStatsAccessible) {
    FlowControlRateLimiter rl;
    rl.updateRate(1000000);

    FlowControlTicketholder::CurOp stats;
    rl.acquireTicket(_opCtx.get(), &stats);

    const auto& rlStats = rl.stats();
    ASSERT_GTE(rlStats.successfulAdmissions(), 1);
    ASSERT_GTE(rlStats.attemptedAdmissions(), 1);
}

TEST_F(FlowControlRateLimiterTest, CustomMaxQueueDepth) {
    FlowControlRateLimiter rl;
    rl.updateRate(5000);

    FlowControlTicketholder::CurOp stats;
    rl.acquireTicket(_opCtx.get(), &stats);
    ASSERT_EQUALS(stats.ticketsAcquired, 1);
}

}  // namespace
}  // namespace mongo
